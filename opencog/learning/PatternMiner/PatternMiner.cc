/*
 * opencog/learning/PatternMiner/PatternMiner.cc
 *
 * Copyright (C) 2012 by OpenCog Foundation
 * All Rights Reserved
 *
 * Written by Shujing Ke <rainkekekeke@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License v3 as
 * published by the Free Software Foundation and including the exceptions
 * at http://opencog.org/wiki/Licenses
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program; if not, write to:
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <math.h>
#include <stdlib.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <vector>
#include <sstream>
#include <thread>

#include <opencog/atoms/base/ClassServer.h>
#include <opencog/atoms/base/Handle.h>
#include <opencog/atoms/base/atom_types.h>
#include <opencog/spacetime/atom_types.h>
#include <opencog/embodiment/atom_types.h>
//#include <opencog/atoms/bind/BindLink.h>
#include <opencog/query/BindLinkAPI.h>
#include <opencog/util/Config.h>
#include <opencog/util/StringManipulator.h>

#include "PatternMiner.h"

using namespace opencog::PatternMining;
using namespace opencog;

const string PatternMiner::ignoreKeyWords[] = {"this", "that","these","those","it","he", "him", "her", "she" };

void PatternMiner::generateIndexesOfSharedVars(Handle& link, vector<Handle>& orderedHandles, vector < vector<int> >& indexes)
{
    HandleSeq outgoingLinks = atomSpace->get_outgoing(link);
    for (Handle h : outgoingLinks)
    {
        if (atomSpace->is_node(h))
        {
            if (atomSpace->get_type(h) == opencog::VARIABLE_NODE)
            {
                string var_name = atomSpace->get_name(h);

                vector<int> indexesForCurVar;
                int index = 0;

                for (Handle oh : orderedHandles)
                {
                    string ohStr = atomSpace->atom_as_string(oh);
                    if (ohStr.find(var_name) != std::string::npos)
                    {
                        indexesForCurVar.push_back(index);
                    }

                    index ++;
                }

                indexes.push_back(indexesForCurVar);
            }
        }
        else
            generateIndexesOfSharedVars(h,orderedHandles,indexes);
    }
}

void PatternMiner::findAndRenameVariablesForOneLink(Handle link, map<Handle,Handle>& varNameMap, HandleSeq& renameOutgoingLinks)
{

    HandleSeq outgoingLinks = atomSpace->get_outgoing(link);

    for (Handle h : outgoingLinks)
    {

        if (atomSpace->is_node(h))
        {
           if (atomSpace->get_type(h) == opencog::VARIABLE_NODE)
           {
               // it's a variable node, rename it
               if (varNameMap.find(h) != varNameMap.end())
               {
                   renameOutgoingLinks.push_back(varNameMap[h]);
               }
               else
               {
                   string var_name = "$var_"  + toString(varNameMap.size() + 1);
                   Handle var_node = atomSpace->add_node(opencog::VARIABLE_NODE, var_name);
                   // XXX why do we need to set the TV ???
                   var_node->merge(TruthValue::TRUE_TV());
                   varNameMap.insert(std::pair<Handle,Handle>(h,var_node));
                   renameOutgoingLinks.push_back(var_node);
               }
           }
           else
           {
               // it's a const node, just add it
               renameOutgoingLinks.push_back(h);

           }
        }
        else
        {
             HandleSeq _renameOutgoingLinks;
             findAndRenameVariablesForOneLink(h, varNameMap, _renameOutgoingLinks);
             Handle reLink = atomSpace->add_link(atomSpace->get_type(h),_renameOutgoingLinks);
             // XXX why do we need to set the TV ???
             reLink->merge(TruthValue::TRUE_TV());
             renameOutgoingLinks.push_back(reLink);
        }

    }

}

vector<Handle> PatternMiner::RebindVariableNames(vector<Handle>& orderedPattern, map<Handle,Handle>& orderedVarNameMap)
{

    vector<Handle> rebindedPattern;

    for (Handle link : orderedPattern)
    {
        HandleSeq renameOutgoingLinks;
        findAndRenameVariablesForOneLink(link, orderedVarNameMap, renameOutgoingLinks);
        Handle rebindedLink = atomSpace->add_link(atomSpace->get_type(link),renameOutgoingLinks);
        // XXX why do we need to set the TV ???
        rebindedLink->merge(TruthValue::TRUE_TV());
        rebindedPattern.push_back(rebindedLink);
    }

    return rebindedPattern;
}

// the input links should be like: only specify the const node, all the variable node name should not be specified:
// unifiedLastLinkIndex is to return where the last link in the input pattern is now in the ordered pattern
// because the last link in input pattern is the externed link from last gram pattern
vector<Handle> PatternMiner::UnifyPatternOrder(vector<Handle>& inputPattern, unsigned int& unifiedLastLinkIndex)
{

    // Step 1: take away all the variable names, make the pattern into such format string:
    //    (InheritanceLink
    //       (VariableNode )
    //       (ConceptNode "Animal")

    //    (InheritanceLink
    //       (VariableNode )
    //       (VariableNode )

    //    (InheritanceLink
    //       (VariableNode )
    //       (VariableNode )

    //    (EvaluationLink (stv 1 1)
    //       (PredicateNode "like_food")
    //       (ListLink
    //          (VariableNode )
    //          (ConceptNode "meat")
    //       )
    //    )

    multimap<string, Handle> nonVarStrToHandleMap;

    for (Handle inputH : inputPattern)
    {
        string str = atomSpace->atom_as_string(inputH);
        string nonVarNameString = "";
        std::stringstream stream(str);
        string oneLine;

        while(std::getline(stream, oneLine,'\n'))
        {
            if (oneLine.find("VariableNode")==std::string::npos)
            {
                // this node is not a VariableNode, just keep this line
                nonVarNameString += oneLine;
            }
            else
            {
                // this node is an VariableNode, remove the name, just keep "VariableNode"
                nonVarNameString += "VariableNode";
            }
        }

        nonVarStrToHandleMap.insert(std::pair<string, Handle>(nonVarNameString,inputH));
    }

    // Step 2: sort the order of all the handls do not share the same string key with other handls
    // becasue the strings are put into a map , so they are already sorted.
    // now print the Handles that do not share the same key with other Handles into a vector, left the Handles share the same keys

    vector<Handle> orderedHandles;
    vector<string> duplicateStrs;
    multimap<string, Handle>::iterator it;
    for(it = nonVarStrToHandleMap.begin(); it != nonVarStrToHandleMap.end();)
    {
        int count = nonVarStrToHandleMap.count(it->first);
        if (count == 1)
        {
            // if this key string has only one record , just put the corresponding handle to the end of orderedHandles
            orderedHandles.push_back(it->second);
            it ++;
        }
        else
        {
            // this key string has multiple handles to it, not put these handles into the orderedHandles,
            // insteadly put this key string into duplicateStrs
            duplicateStrs.push_back(it->first);
            it = nonVarStrToHandleMap.upper_bound(it->first);
        }

    }

    // Step 3: sort the order of the handls share the same string key with other handles
    for (string keyString : duplicateStrs)
    {
        // get all the corresponding handles for this key string
        multimap<string, Handle>::iterator kit;
        vector<_non_ordered_pattern> sharedSameKeyPatterns;
        for (kit = nonVarStrToHandleMap.lower_bound(keyString); kit != nonVarStrToHandleMap.upper_bound(keyString);  ++ kit)
        {
            _non_ordered_pattern p;
            p.link = kit->second;
            generateIndexesOfSharedVars(p.link, orderedHandles, p.indexesOfSharedVars);
            sharedSameKeyPatterns.push_back(p);
        }

        std::sort(sharedSameKeyPatterns.begin(), sharedSameKeyPatterns.end());
        for (_non_ordered_pattern np : sharedSameKeyPatterns)
        {
            orderedHandles.push_back(np.link);
        }
    }

    // find out where the last link in the input pattern is now in the ordered pattern
    Handle lastLink = inputPattern[inputPattern.size()-1];
    unsigned int lastLinkIndex = 0;
    for (Handle h : orderedHandles)
    {
        if (h == lastLink)
        {
            unifiedLastLinkIndex = lastLinkIndex;
            break;
        }

        ++ lastLinkIndex;

    }

    // in this map, the first Handle is the variable node is the original Atomspace,
    // the second Handle is the renamed ordered variable node in the Pattern Mining Atomspace.
    map<Handle,Handle> orderedVarNameMap;

    vector<Handle> rebindPattern = RebindVariableNames(orderedHandles, orderedVarNameMap);

    return rebindPattern;

}

string PatternMiner::unifiedPatternToKeyString(vector<Handle>& inputPattern, const AtomSpace *atomspace)
{
    if (atomspace == 0)
        atomspace = this->atomSpace;

    string keyStr = "";
    for (Handle h : inputPattern)
    {
        keyStr += Link2keyString(h,"", atomspace);
        keyStr += "\n";
    }

    return keyStr;
}

bool PatternMiner::checkPatternExist(const string& patternKeyStr)
{
    if (keyStrToHTreeNodeMap.find(patternKeyStr) == keyStrToHTreeNodeMap.end())
        return false;
    else
        return true;

}

void PatternMiner::generateNextCombinationGroup(bool* &indexes, int n_max)
{
    int trueCount = -1;
    int i = 0;
    for (; i < n_max - 1; ++ i)
    {
        if (indexes[i])
        {
            ++ trueCount;

            if (! indexes[i+1])
                break;
        }
    }

    indexes[i] = false;
    indexes[i+1] = true;

    for (int j = 0; j < trueCount; ++ j)
        indexes[j] = true;

    for (int j = trueCount; j < i; ++ j)
        indexes[j] = false;

}

bool PatternMiner::isLastNElementsAllTrue(bool* array, int size, int n)
{
    for (int i = size - 1; i >= size - n; i --)
    {
        if (! array[i])
            return false;
    }

    return true;
}

unsigned int combinationCalculate(int r, int n)
{
    // = n!/(r!*(n-r)!)
    int top = 1;
    for (int i = n; i > r; i-- )
    {
        top *= i;
    }

    int bottom = 1;
    for (int j = n-r; j > 1; j--)
    {
        bottom *= j;
    }

    return top/bottom;

}

 // valueToVarMap:  the ground value node in the orginal Atomspace to the variable handle in pattenmining Atomspace
void PatternMiner::generateALinkByChosenVariables(Handle& originalLink, map<Handle,Handle>& valueToVarMap,  HandleSeq& outputOutgoings,AtomSpace *_fromAtomSpace)
{
    HandleSeq outgoingLinks = _fromAtomSpace->get_outgoing(originalLink);

    for (Handle h : outgoingLinks)
    {
        if (_fromAtomSpace->is_node(h))
        {
           if (valueToVarMap.find(h) != valueToVarMap.end())
           {
               // this node is considered as a variable
               outputOutgoings.push_back(valueToVarMap[h]);
           }
           else
           {
               // this node is considered not a variable, so add its bound value node into the Pattern mining Atomspace
               Handle value_node = atomSpace->add_node(_fromAtomSpace->get_type(h), _fromAtomSpace->get_name(h));
               // XXX why do we need to set the TV ???
               value_node->merge(TruthValue::TRUE_TV());
               outputOutgoings.push_back(value_node);
           }
        }
        else
        {
             HandleSeq _outputOutgoings;
             generateALinkByChosenVariables(h, valueToVarMap, _outputOutgoings, _fromAtomSpace);
             Handle reLink = atomSpace->add_link(_fromAtomSpace->get_type(h),_outputOutgoings);
             // XXX why do we need to set the TV ???
             reLink->merge(TruthValue::TRUE_TV());
             outputOutgoings.push_back(reLink);
        }
    }
}

 // valueToVarMap:  the ground value node in the orginal Atomspace to the variable handle in pattenmining Atomspace
// _fromAtomSpace: where is input link from
void PatternMiner::extractAllNodesInLink(Handle link, map<Handle,Handle>& valueToVarMap, AtomSpace* _fromAtomSpace)
{
    HandleSeq outgoingLinks = _fromAtomSpace->get_outgoing(link);

    for (Handle h : outgoingLinks)
    {
        if (_fromAtomSpace->is_node(h))
        {
            if (valueToVarMap.find(h) == valueToVarMap.end())
            {
                // add a variable node in Pattern miner Atomspace
                Handle varHandle = atomSpace->add_node(opencog::VARIABLE_NODE,"$var~" + toString(valueToVarMap.size()) );
                valueToVarMap.insert(std::pair<Handle,Handle>(h,varHandle));
            }

            if ((_fromAtomSpace->get_type(h) == opencog::VARIABLE_NODE))
                cout<<"Error: instance link contains variables: \n" << _fromAtomSpace->atom_as_string(h)<<std::endl;

        }
        else
        {
            extractAllNodesInLink(h,valueToVarMap,_fromAtomSpace);
        }
    }
}

void PatternMiner::extractAllVariableNodesInAnInstanceLink(Handle& instanceLink, Handle& patternLink, set<Handle>& allVarNodes)
{

    HandleSeq ioutgoingLinks = originalAtomSpace->get_outgoing(instanceLink);
    HandleSeq poutgoingLinks = atomSpace->get_outgoing(patternLink);

    HandleSeq::iterator pit = poutgoingLinks.begin();

    for (Handle h : ioutgoingLinks)
    {
        if (originalAtomSpace->is_node(h))
        {
            if ((atomSpace->get_type(*pit) == opencog::VARIABLE_NODE))
            {
                if (allVarNodes.find(h) == allVarNodes.end())
                {
                    allVarNodes.insert(h);
                }
            }
        }
        else
        {
            extractAllVariableNodesInAnInstanceLink(h,(Handle&)(*pit),allVarNodes);
        }

        pit ++;
    }

}


void PatternMiner::extractAllVariableNodesInAnInstanceLink(Handle& instanceLink, Handle& patternLink, map<Handle, unsigned int>& allVarNodes, unsigned index)
{

    HandleSeq ioutgoingLinks = originalAtomSpace->get_outgoing(instanceLink);
    HandleSeq poutgoingLinks = atomSpace->get_outgoing(patternLink);

    HandleSeq::iterator pit = poutgoingLinks.begin();

    for (Handle h : ioutgoingLinks)
    {
        if (originalAtomSpace->is_node(h))
        {
            if ((atomSpace->get_type(*pit) == opencog::VARIABLE_NODE))
            {
                if (allVarNodes.find(h) == allVarNodes.end())
                {
                    allVarNodes.insert(std::pair<Handle, unsigned>(h,index));
                }
            }
        }
        else
        {
            extractAllVariableNodesInAnInstanceLink(h,(Handle&)(*pit),allVarNodes, index);
        }

        pit ++;
    }

}

// map<Handle, unsigned> allNodes  , unsigned is the index the first link this node belongs to
void PatternMiner::extractAllNodesInLink(Handle link, map<Handle, unsigned int>& allNodes, AtomSpace* _fromAtomSpace, unsigned index)
{
    HandleSeq outgoingLinks = _fromAtomSpace->get_outgoing(link);

    for (Handle h : outgoingLinks)
    {
        if (_fromAtomSpace->is_node(h))
        {
            if (allNodes.find(h) == allNodes.end())
            {
                allNodes.insert(std::pair<Handle, unsigned>(h,index) );
            }
        }
        else
        {
            extractAllNodesInLink(h,allNodes,_fromAtomSpace, index);
        }
    }
}

void PatternMiner::extractAllNodesInLink(Handle link, set<Handle>& allNodes, AtomSpace* _fromAtomSpace)
{
    HandleSeq outgoingLinks = _fromAtomSpace->get_outgoing(link);

    for (Handle h : outgoingLinks)
    {
        if (_fromAtomSpace->is_node(h))
        {
            if (allNodes.find(h) == allNodes.end())
            {
                allNodes.insert(h);
            }
        }
        else
        {
            extractAllNodesInLink(h,allNodes,_fromAtomSpace);
        }
    }
}

void PatternMiner::extractAllVariableNodesInLink(Handle link, set<Handle>& allNodes, AtomSpace* _atomSpace)
{
    HandleSeq outgoingLinks = _atomSpace->get_outgoing(link);

    for (Handle h : outgoingLinks)
    {
        if (_atomSpace->is_node(h))
        {
            if ((_atomSpace->get_type(h) == opencog::VARIABLE_NODE) && (allNodes.find(h) == allNodes.end()))
            {
                allNodes.insert(h);
            }
        }
        else
        {
            extractAllVariableNodesInLink(h,allNodes, _atomSpace);
        }
    }
}

bool PatternMiner::onlyContainVariableNodes(Handle link, AtomSpace* _atomSpace)
{
    HandleSeq outgoingLinks = _atomSpace->get_outgoing(link);

    for (Handle h : outgoingLinks)
    {
        if (_atomSpace->is_node(h))
        {
            if (_atomSpace->get_type(h) != opencog::VARIABLE_NODE)
            {
                return false;
            }
        }
        else
        {
            if (! onlyContainVariableNodes(h, _atomSpace))
                return false;
        }
    }

    return true;
}




void PatternMiner::swapOneLinkBetweenTwoAtomSpace(AtomSpace* fromAtomSpace, AtomSpace* toAtomSpace, Handle& fromLink, HandleSeq& outgoings,
                                                  HandleSeq &outVariableNodes )
{
    HandleSeq outgoingLinks = fromAtomSpace->get_outgoing(fromLink);

    for (Handle h : outgoingLinks)
    {
        if (fromAtomSpace->is_node(h))
        {
           Handle new_node = toAtomSpace->add_node(fromAtomSpace->get_type(h), fromAtomSpace->get_name(h));
           new_node->merge(fromAtomSpace->get_TV(h));
           outgoings.push_back(new_node);
           if (fromAtomSpace->get_type(h) == VARIABLE_NODE)
           {
               if ( ! isInHandleSeq(new_node, outVariableNodes) ) // should not have duplicated variable nodes
                outVariableNodes.push_back(new_node);
           }
        }
        else
        {
             HandleSeq _OutgoingLinks;

             swapOneLinkBetweenTwoAtomSpace(fromAtomSpace, toAtomSpace, h, _OutgoingLinks, outVariableNodes);
             Handle _link = toAtomSpace->add_link(fromAtomSpace->get_type(h), _OutgoingLinks);
             _link->merge(fromAtomSpace->get_TV(h));

             outgoings.push_back(_link);
        }
    }
}

// linksWillBeDel are all the links contain varaibles. Those links need to be deleted after run BindLink
HandleSeq PatternMiner::swapLinksBetweenTwoAtomSpace(AtomSpace* fromAtomSpace, AtomSpace* toAtomSpace, HandleSeq& fromLinks, HandleSeq& outVariableNodes)
{
    HandleSeq outPutLinks;

    for (Handle link : fromLinks)
    {
        HandleSeq outgoingLinks;

        swapOneLinkBetweenTwoAtomSpace(fromAtomSpace, toAtomSpace, link, outgoingLinks, outVariableNodes);
        Handle toLink = toAtomSpace->add_link(fromAtomSpace->get_type(link), outgoingLinks);
        toLink->merge(fromAtomSpace->get_TV(link));

        outPutLinks.push_back(toLink);
    }

    return outPutLinks;
}

 // using PatternMatcher
void PatternMiner::findAllInstancesForGivenPatternInNestedAtomSpace(HTreeNode* HNode)
{

//     First, generate the Bindlink for using PatternMatcher to find all the instances for this pattern in the original Atomspace
//    (BindLink
//        ;; The variables to be bound
//        (Listlink)
//          (VariableNode "$var_1")
//          (VariableNode "$var_2")
//          ...
//        (ImplicationLink
//          ;; The pattern to be searched for
//          (pattern)
//          (Listlink)
//              ;; The instance to be returned.
//              (result)
//              (variable Listlink)
//        )
//     )

    HandleSeq  implicationLinkOutgoings, bindLinkOutgoings;

    // HandleSeq patternToMatch = swapLinksBetweenTwoAtomSpace(atomSpace, originalAtomSpace, HNode->pattern, variableNodes, linksWillBeDel);

//    if (HNode->pattern.size() == 1) // this pattern only contains one link
//    {
//        implicationLinkOutgoings.push_back(patternToMatch[0]); // the pattern to match
//        implicationLinkOutgoings.push_back(patternToMatch[0]); // the results to return

//        std::cout<<"Debug: PatternMiner::findAllInstancesForGivenPattern for pattern:" << std::endl
//                << originalAtomSpace->atom_as_string(patternToMatch[0]).c_str() << std::endl;
//    }

    Handle hAndLink = atomSpace->add_link(AND_LINK, HNode->pattern);
    // XXX why do we need to set the TV ???
    hAndLink->merge(TruthValue::TRUE_TV());
    Handle hOutPutListLink = atomSpace->add_link(LIST_LINK, HNode->pattern);
    // XXX why do we need to set the TV ???
    hOutPutListLink->merge(TruthValue::TRUE_TV());
    implicationLinkOutgoings.push_back(hAndLink); // the pattern to match
    implicationLinkOutgoings.push_back(hOutPutListLink); // the results to return

//    std::cout <<"Debug: PatternMiner::findAllInstancesForGivenPattern for pattern:" << std::endl
//            << atomSpace->atom_as_string(hAndLink).c_str() << std::endl;


    Handle hImplicationLink = atomSpace->add_link(IMPLICATION_LINK, implicationLinkOutgoings);
    hImplicationLink->merge(TruthValue::TRUE_TV());

    // add variable atoms
    set<Handle> allVariableNodesInPattern;
    for (unsigned int i = 0; i < HNode->pattern.size(); ++i)
    {
        extractAllVariableNodesInLink(HNode->pattern[i],allVariableNodesInPattern, atomSpace);
    }

    HandleSeq variableNodes;
    for (Handle varh : allVariableNodesInPattern)
    {
        Handle v = atomSpace->add_node(VARIABLE_NODE, atomSpace->get_name(varh));
        variableNodes.push_back(v);
    }

    Handle hVariablesListLink = atomSpace->add_link(LIST_LINK, variableNodes);
    // XXX why do we need to set the TV ???
    hVariablesListLink->merge(TruthValue::TRUE_TV());

    bindLinkOutgoings.push_back(hVariablesListLink);
    bindLinkOutgoings.push_back(hImplicationLink);
    Handle hBindLink = atomSpace->add_link(BIND_LINK, bindLinkOutgoings);
    // XXX why do we need to set the TV ???
    hBindLink->merge(TruthValue::TRUE_TV());


    string s = atomSpace->atom_as_string(hBindLink);

    // Run pattern matcher
    Handle hResultListLink = opencog::bindlink(atomSpace, hBindLink);


    // Get result
    // Note: Don't forget to remove the hResultListLink and BindLink
    HandleSeq resultSet = atomSpace->get_outgoing(hResultListLink);

//     std::cout << toString(resultSet.size())  << " instances found!" << std::endl ;

    //    //debug
//    std::cout << atomSpace->atom_as_string(hResultListLink) << std::endl  << std::endl;

    atomSpace->remove_atom(hResultListLink);

    for (Handle listH  : resultSet)
    {
        HandleSeq instanceLinks = atomSpace->get_outgoing(listH);

        if (cur_gram == 1)
        {
            HNode->instances.push_back(instanceLinks);
        }
        else
        {
            // instance that contains duplicate links will not be added
            if (! containsDuplicateHandle(instanceLinks))
                HNode->instances.push_back(instanceLinks);
        }

        atomSpace->remove_atom(listH);
    }

    atomSpace->remove_atom(hBindLink);
    atomSpace->remove_atom(hImplicationLink);
    atomSpace->remove_atom(hAndLink);
    atomSpace->remove_atom(hOutPutListLink);

//    atomSpace->remove_atom(hVariablesListLink);

    HNode->count = HNode->instances.size();
}


void PatternMiner::removeLinkAndItsAllSubLinks(AtomSpace* _atomspace, Handle link)
{

//    //debug
//    std::cout << "Remove atom: " << _atomspace->atom_as_string(link) << std::endl;

    HandleSeq Outgoings = _atomspace->get_outgoing(link);
    for (Handle h : Outgoings)
    {
        if (_atomspace->is_link(h))
        {
            removeLinkAndItsAllSubLinks(_atomspace,h);

        }
    }
    _atomspace->remove_atom(link);

}



bool PatternMiner::containsDuplicateHandle(HandleSeq &handles)
{
    for (unsigned int i = 0; i < handles.size(); i ++)
    {
        for (unsigned int j = i+1; j < handles.size(); j ++)
        {
            if (handles[j] == handles[i])
                return true;
        }
    }

    return false;
}


bool PatternMiner::isInHandleSeq(Handle handle, HandleSeq &handles)
{
    for (Handle h : handles)
    {
        if (handle == h)
            return true;
    }

    return false;
}

bool PatternMiner::isInHandleSeqSeq(Handle handle, HandleSeqSeq &handleSeqs)
{
    for (HandleSeq handles : handleSeqs)
    {
        if (isInHandleSeq(handle,handles))
            return true;
    }

    return false;
}

bool PatternMiner::isIgnoredType(Type type)
{
    for (Type t : ignoredTypes)
    {
        if (t == type)
            return true;
    }

    return false;
}

Handle PatternMiner::getFirstNonIgnoredIncomingLink(AtomSpace *atomspace, Handle& handle)
{
    Handle cur_h = handle;
    while(true)
    {
        HandleSeq incomings = atomspace->get_incoming(cur_h);
        if (incomings.size() == 0)
            return Handle::UNDEFINED;

        if (isIgnoredType (atomspace->get_type(incomings[0])))
        {
            cur_h = incomings[0];
            continue;
        }
        else
            return incomings[0];

    }

}


bool compareHTreeNodeByFrequency(HTreeNode* node1, HTreeNode* node2)
{
    return (node1->count > node2->count);
}

bool compareHTreeNodeByInteractionInformation(HTreeNode* node1, HTreeNode* node2)
{
    return (node1->interactionInformation > node2->interactionInformation);
}

bool compareHTreeNodeBySurprisingness(HTreeNode* node1, HTreeNode* node2)
{
    if ( (node1->nI_Surprisingness +  node1->nII_Surprisingness) - (node2->nI_Surprisingness + node2->nII_Surprisingness) > FLOAT_MIN_DIFF)
        return true;
    else if ((node2->nI_Surprisingness + node2->nII_Surprisingness) - (node1->nI_Surprisingness +  node1->nII_Surprisingness) > FLOAT_MIN_DIFF)
        return false;

    return (node1->var_num < node2->var_num);
}

bool compareHTreeNodeBySurprisingness_I(HTreeNode* node1, HTreeNode* node2)
{
    if ( node1->nI_Surprisingness - node2->nI_Surprisingness  > FLOAT_MIN_DIFF)
        return true;
    else if (node2->nI_Surprisingness - node1->nI_Surprisingness > FLOAT_MIN_DIFF)
        return false;

    return (node1->var_num < node2->var_num);
}

bool compareHTreeNodeBySurprisingness_II(HTreeNode* node1, HTreeNode* node2)
{
    if ( node1->nII_Surprisingness - node2->nII_Surprisingness > FLOAT_MIN_DIFF)
        return true;
    else if ( node2->nII_Surprisingness - node1->nII_Surprisingness > FLOAT_MIN_DIFF)
        return false;

    return (node1->var_num < node2->var_num);
}


// only used by Surprisingness evaluation mode
void PatternMiner::OutPutFinalPatternsToFile(unsigned int n_gram)
{

    // out put the n_gram patterns to a file
    ofstream resultFile;
    string fileName  = "FinalTopPatterns_" + toString(n_gram) + "gram.scm";
    std::cout<<"\nDebug: PatternMiner: writing (gram = " + toString(n_gram) + ") final top patterns to file " + fileName << std::endl;

    resultFile.open(fileName.c_str());
    vector<HTreeNode*> &patternsForThisGram = finalPatternsForGram[n_gram-1];


    resultFile << "Interesting Pattern Mining final results for " + toString(n_gram) + " gram patterns. Total pattern number: " + toString(patternsForThisGram.size()) << endl;


    for (HTreeNode* htreeNode : patternsForThisGram)
    {
        if (htreeNode->count < 2)
            continue;

        resultFile << endl << "Pattern: Frequency = " << toString(htreeNode->count);

        resultFile << ", SurprisingnessI = " << toString(htreeNode->nI_Surprisingness);

        resultFile << ", SurprisingnessII = " << toString(htreeNode->nII_Surprisingness);


        resultFile << endl;

        resultFile << unifiedPatternToKeyString(htreeNode->pattern)<< endl;


    }

    resultFile.close();


}



void PatternMiner::OutPutFrequentPatternsToFile(unsigned int n_gram)
{

    // out put the n_gram frequent patterns to a file, in the order of frequency
    ofstream resultFile;
    string fileName = "FrequentPatterns_" + toString(n_gram) + "gram.scm";

    std::cout<<"\nDebug: PatternMiner: writing  (gram = " + toString(n_gram) + ") frequent patterns to file " + fileName << std::endl;

    resultFile.open(fileName.c_str());
    vector<HTreeNode*> &patternsForThisGram = patternsForGram[n_gram-1];

    resultFile << "Frequent Pattern Mining results for " + toString(n_gram) + " gram patterns. Total pattern number: " + toString(patternsForThisGram.size()) << endl;

    for (HTreeNode* htreeNode : patternsForThisGram)
    {
        if (htreeNode->count < 2)
            continue;

        resultFile << endl << "Pattern: Frequency = " << toString(htreeNode->count);

        resultFile << endl;

        resultFile << unifiedPatternToKeyString(htreeNode->pattern)<< endl;

//        for (Handle link : htreeNode->pattern)
//        {
//            resultFile << atomSpace->atom_as_string(link);
//        }
    }

    resultFile.close();


}

void PatternMiner::OutPutInterestingPatternsToFile(vector<HTreeNode*> &patternsForThisGram, unsigned int n_gram, int surprisingness) // surprisingness 1 or 2, it is default 0 which means Interaction_Information
{

    // out put the n_gram patterns to a file
    ofstream resultFile;
    string fileName;

    if (interestingness_Evaluation_method == "Interaction_Information")
        fileName = "Interaction_Information_" + toString(n_gram) + "gram.scm";
    else if (surprisingness == 1)
        fileName = "SurprisingnessI_" + toString(n_gram) + "gram.scm";
    else
        fileName = "SurprisingnessII_" + toString(n_gram) + "gram.scm";

    std::cout<<"\nDebug: PatternMiner: writing (gram = " + toString(n_gram) + ") interesting patterns to file " + fileName << std::endl;

    resultFile.open(fileName.c_str());


    resultFile << "Interesting Pattern Mining results for " + toString(n_gram) + " gram patterns. Total pattern number: " + toString(patternsForThisGram.size()) << endl;

    for (HTreeNode* htreeNode : patternsForThisGram)
    {
        if (htreeNode->count < 2)
            continue;

        resultFile << endl << "Pattern: Frequency = " << toString(htreeNode->count);

        if (interestingness_Evaluation_method == "Interaction_Information")
            resultFile << " InteractionInformation = " << toString(htreeNode->interactionInformation);
        else if (interestingness_Evaluation_method == "surprisingness")
        {
            if (surprisingness == 1)
                resultFile << " SurprisingnessI = " << toString(htreeNode->nI_Surprisingness);
            else
                resultFile << " SurprisingnessII = " << toString(htreeNode->nII_Surprisingness);
        }


        resultFile << endl;

        resultFile << unifiedPatternToKeyString(htreeNode->pattern)<< endl;

//        for (Handle link : htreeNode->pattern)
//        {
//            resultFile << atomSpace->atom_as_string(link);
//        }
    }

    resultFile.close();


}


// call this function only after sort by frequency
void PatternMiner::OutPutStaticsToCsvFile(unsigned int n_gram)
{
    // out put to csv file
    ofstream csvFile;
    string csvfileName = "PatternStatics_" + toString(n_gram) + "gram.csv";

    vector<HTreeNode*> &patternsForThisGram = patternsForGram[n_gram-1];

    std::cout<<"\nDebug: PatternMiner: writing  (gram = " + toString(n_gram) + ") pattern statics to csv file " + csvfileName << std::endl;

    csvFile.open(csvfileName.c_str());


    csvFile << "Frequency,Surprisingness_I,Surprisingness_II" << std::endl;

    for (HTreeNode* htreeNode : patternsForThisGram)
    {
        if (htreeNode->count < 2)
            continue;

        csvFile << htreeNode->count << "," << htreeNode->nI_Surprisingness << ",";

        if (htreeNode->superPatternRelations.size() > 0)
            csvFile << htreeNode->nII_Surprisingness;
        else
            csvFile << "unknown";

        csvFile << std::endl;
    }

    csvFile.close();
}

void PatternMiner::OutPutLowFrequencyHighSurprisingnessPatternsToFile(vector<HTreeNode*> &patternsForThisGram, unsigned int n_gram)
{

    // out put the n_gram patterns to a file
    ofstream resultFile;
    string fileName;


    fileName = "LowFrequencyHighSurprisingness_" + toString(n_gram) + "gram.scm";

    std::cout<<"\nDebug: PatternMiner: writing (gram = " + toString(n_gram) + ") LowFrequencyHighSurprisingness patterns to file " + fileName << std::endl;


    vector<HTreeNode*> resultPatterns;

    for (HTreeNode* htreeNode : patternsForThisGram)
    {
        if ( (htreeNode->count < 4) && (htreeNode->count > 1))
            resultPatterns.push_back(htreeNode);
    }

    std::sort(resultPatterns.begin(), resultPatterns.end(),compareHTreeNodeBySurprisingness_I);

    resultFile.open(fileName.c_str());

    resultFile << "Interesting Pattern Mining results for " + toString(n_gram) + " gram patterns. Total pattern number: " + toString(resultPatterns.size()) << endl;

    resultFile << "This file contains the pattern with Frequency < 4, sort by Surprisingness_I"  << std::endl;


    for (HTreeNode* htreeNode : resultPatterns)
    {
        resultFile << endl << "Pattern: Frequency = " << toString(htreeNode->count);

        resultFile << " SurprisingnessI = " << toString(htreeNode->nI_Surprisingness);

        resultFile << " SurprisingnessII = " << toString(htreeNode->nII_Surprisingness);

        resultFile << endl;

        resultFile << unifiedPatternToKeyString(htreeNode->pattern)<< endl;


    }

    resultFile.close();


}

void PatternMiner::OutPutHighFrequencyHighSurprisingnessPatternsToFile(vector<HTreeNode*> &patternsForThisGram, unsigned int n_gram, unsigned int min_frequency)
{

    // out put the n_gram patterns to a file
    ofstream resultFile;
    string fileName;


    fileName = "HighFrequencyHighSurprisingness_" + toString(n_gram) + "gram.scm";

    std::cout<<"\nDebug: PatternMiner: writing (gram = " + toString(n_gram) + ") HighFrequencyHighSurprisingness patterns to file " + fileName << std::endl;

    vector<HTreeNode*> resultPatterns;

    for (HTreeNode* htreeNode : patternsForThisGram)
    {
        if (htreeNode->count > min_frequency)
            resultPatterns.push_back(htreeNode);
    }

    std::sort(resultPatterns.begin(), resultPatterns.end(),compareHTreeNodeBySurprisingness_I);


    resultFile.open(fileName.c_str());


    resultFile << "Interesting Pattern Mining results for " + toString(n_gram) + " gram patterns. Total pattern number: " + toString(resultPatterns.size()) << endl;

    resultFile << "This file contains the pattern with Frequency > " << min_frequency << ", sort by Surprisingness_I"  << std::endl;


    for (HTreeNode* htreeNode : resultPatterns)
    {
        resultFile << endl << "Pattern: Frequency = " << toString(htreeNode->count);

        resultFile << " SurprisingnessI = " << toString(htreeNode->nI_Surprisingness);

        resultFile << " SurprisingnessII = " << toString(htreeNode->nII_Surprisingness);

        resultFile << endl;

        resultFile << unifiedPatternToKeyString(htreeNode->pattern)<< endl;


    }

    resultFile.close();


}

void PatternMiner::OutPutHighSurprisingILowSurprisingnessIIPatternsToFile(vector<HTreeNode*> &patternsForThisGram,unsigned int n_gram, float min_surprisingness_I, float max_surprisingness_II)
{

    // out put the n_gram patterns to a file
    ofstream resultFile;
    string fileName;


    fileName = "HighSurprisingILowSurprisingnessII" + toString(n_gram) + "gram.scm";

    std::cout<<"\nDebug: PatternMiner: writing (gram = " + toString(n_gram) + ") HighSurprisingILowSurprisingnessII patterns to file " + fileName << std::endl;

    vector<HTreeNode*> resultPatterns;

    for (HTreeNode* htreeNode : patternsForThisGram)
    {
        if (htreeNode->superPatternRelations.size() == 0)
            continue;

        if (htreeNode->count < 10)
            continue;

        if ( (htreeNode->nI_Surprisingness > min_surprisingness_I) && (htreeNode->nII_Surprisingness < max_surprisingness_II) )
            resultPatterns.push_back(htreeNode);
    }


    resultFile.open(fileName.c_str());


    resultFile << "Interesting Pattern Mining results for " + toString(n_gram) + " gram patterns. Total pattern number: " + toString(resultPatterns.size()) << endl;

    resultFile << "This file contains the pattern with Surprising_I > " << min_surprisingness_I << ", and Surprisingness_II < "  << max_surprisingness_II << std::endl;


    // std::sort(resultPatterns.begin(), resultPatterns.end(),compareHTreeNodeBySurprisingness_I);

    for (HTreeNode* htreeNode : resultPatterns)
    {
        resultFile << endl << "Pattern: Frequency = " << toString(htreeNode->count);

        resultFile << " SurprisingnessI = " << toString(htreeNode->nI_Surprisingness);

        resultFile << " SurprisingnessII = " << toString(htreeNode->nII_Surprisingness);

        resultFile << endl;

        resultFile << unifiedPatternToKeyString(htreeNode->pattern)<< endl;


    }

    resultFile.close();


}



// To exclude this kind of patterns:
// $var_3 doesn't really can be a variable, all the links contains it doesn't contains any const nodes, so actually $var_3 is a leaf
// we call variable nodes like $var_3 as "loop variable"
//(InheritanceLink )
//  (ConceptNode Broccoli)
//  (VariableNode $var_1)

//(InheritanceLink )
//  (ConceptNode dragonfruit)
//  (VariableNode $var_2)

//(InheritanceLink )
//  (VariableNode $var_2)
//  (VariableNode $var_3)

//(InheritanceLink )
//  (VariableNode $var_1)
//  (VariableNode $var_3)

struct VariableInLinks
{
    HandleSeq onlyContainsVarLinks;
    HandleSeq containsConstLinks;
};

bool PatternMiner::containsLoopVariable(HandleSeq& inputPattern)
{
    // Need no check when gram < 3, it will already be filtered by the leaves filter
    if (inputPattern.size() < 3)
        return false;


    // First find those links that only contains variables, without any const
    map<string, VariableInLinks> varInLinksMap;

    bool allLinksContainsConst = true;

    for (Handle inputH : inputPattern)
    {
        string str = atomSpace->atom_as_string(inputH);
        std::stringstream stream(str);
        string oneLine;
        bool containsOnlyVars = true;
        vector<string> allVarsInThis_link;

        while(std::getline(stream, oneLine,'\n'))
        {
            if (oneLine.find("Node") == std::string::npos) // this line is a link, contains no nodes
                continue;

            std::size_t pos_var = oneLine.find("VariableNode");
            if (pos_var == std::string::npos)
            {
                // this node is a const node
                containsOnlyVars = false;
            }
            else
            {
                string keyVarStr = oneLine.substr(pos_var);
                // this node is a VariableNode
                allVarsInThis_link.push_back(keyVarStr);
            }
        }

        if (containsOnlyVars)
        {
            allLinksContainsConst = false;
        }

        for(string varStr : allVarsInThis_link)
        {
            map<string, VariableInLinks>::iterator it = varInLinksMap.find(varStr);
            if ( it == varInLinksMap.end() )
            {
                VariableInLinks varLinks;
                if (containsOnlyVars)
                    varLinks.onlyContainsVarLinks.push_back(inputH);
                else
                    varLinks.containsConstLinks.push_back(inputH);

                varInLinksMap.insert(std::pair<string, VariableInLinks>(varStr, varLinks));
            }
            else
            {
                VariableInLinks& varLinks = it->second;
                if (containsOnlyVars)
                    varLinks.onlyContainsVarLinks.push_back(inputH);
                else
                    varLinks.containsConstLinks.push_back(inputH);
            }

        }

    }

    if (allLinksContainsConst)
        return false;

    map<string, VariableInLinks>::iterator it;
    for (it = varInLinksMap.begin(); it != varInLinksMap.end(); ++ it)
    {
         VariableInLinks& varLinks = it->second;
         if (varLinks.containsConstLinks.size() == 0)
             return true;

    }

    return false;

}


// Each HandleSeq in HandleSeqSeq oneOfEachSeqShouldBeVars is a list of nodes that connect two links in inputLinks,
// at least one node in a HandleSeq should be variable, which means two connected links in inputLinks should be connected by at least a variable
// leaves are those nodes that are not connected to any other links in inputLinks, they should be
// some will be filter out in this phrase, return true to filter out
bool PatternMiner::filters(HandleSeq& inputLinks, HandleSeqSeq& oneOfEachSeqShouldBeVars, HandleSeq& leaves, HandleSeq& shouldNotBeVars, HandleSeq& shouldBeVars, AtomSpace* _atomSpace)
{
    if(inputLinks.size() < 2)
        return false;

    set<Handle> allNodesInEachLink[inputLinks.size()];

    set<Handle> all2ndOutgoingsOfInherlinks;

    // map<predicate, set<value> >
    map<Handle, set<Handle> > predicateToValueOfEvalLinks;

    set<Handle>  all1stOutgoingsOfEvalLinks;


    for (unsigned int i = 0; i < inputLinks.size(); ++i)
    {
        extractAllNodesInLink(inputLinks[i],allNodesInEachLink[i], _atomSpace);

        if (enable_filter_not_inheritant_from_same_var)
        {
            // filter: Any two InheritanceLinks should not share their secondary outgoing nodes
            if (_atomSpace->get_type(inputLinks[i]) == INHERITANCE_LINK)
            {
                Handle secondOutgoing = _atomSpace->get_outgoing(inputLinks[i], 1);
                if (all2ndOutgoingsOfInherlinks.find(secondOutgoing) == all2ndOutgoingsOfInherlinks.end())
                    all2ndOutgoingsOfInherlinks.insert(secondOutgoing);
                else
                    return true;
            }
        }

        if (enable_filter_not_same_var_from_same_predicate || enable_filter_not_all_first_outgoing_const|| enable_filter_first_outgoing_evallink_should_be_var)
        {
            // this filter: Any two EvaluationLinks with the same predicate should not share the same secondary outgoing nodes
            if (_atomSpace->get_type(inputLinks[i]) == EVALUATION_LINK)
            {
                HandleSeq outgoings = _atomSpace->get_outgoing(inputLinks[i]);
                Handle predicateNode = outgoings[0];

                // value node is the last node of the list link
                HandleSeq outgoings2 = _atomSpace->get_outgoing(outgoings[1]);
                Handle valueNode = outgoings2[outgoings2.size() - 1];

                if (enable_filter_not_same_var_from_same_predicate)
                {
                    map<Handle, set<Handle> >::iterator it = predicateToValueOfEvalLinks.find(predicateNode);
                    if (it != predicateToValueOfEvalLinks.end())
                    {
                        set<Handle>& values = (it->second);
                        if (values.find(valueNode) != values.end())
                            return true;
                        else
                            values.insert(valueNode);
                    }
                    else
                    {
                        set<Handle> newValues;
                        newValues.insert(valueNode);
                        predicateToValueOfEvalLinks.insert(std::pair<Handle, set<Handle> >(predicateNode,newValues));
                    }
                }

                // this filter: at least one of all the 1st outgoings of Evaluationlinks should be var
                if ( enable_filter_first_outgoing_evallink_should_be_var || enable_filter_not_all_first_outgoing_const)
                {
                    if (outgoings2.size() > 1)
                    {
                        if(all1stOutgoingsOfEvalLinks.find(outgoings2[0]) == all1stOutgoingsOfEvalLinks.end())
                            all1stOutgoingsOfEvalLinks.insert(outgoings2[0]);
                    }
                }

            }
        }
    }

    if (all1stOutgoingsOfEvalLinks.size() > 0)
    {
        // this filter: all the first outgoing nodes of all evaluation links should be variables
        if (enable_filter_first_outgoing_evallink_should_be_var)
            std::copy(all1stOutgoingsOfEvalLinks.begin(), all1stOutgoingsOfEvalLinks.end(), std::back_inserter(shouldBeVars));
        else if ((enable_filter_not_all_first_outgoing_const) )
        {
            // if enable_filter_first_outgoing_evallink_should_be_var is true, there is no need to enable this filter below
            vector<Handle> all1stOutgoingsOfEvalLinksSeq(all1stOutgoingsOfEvalLinks.begin(), all1stOutgoingsOfEvalLinks.end());
            oneOfEachSeqShouldBeVars.push_back(all1stOutgoingsOfEvalLinksSeq);
        }
    }


    if (enable_filter_links_should_connect_by_vars)
    {
        // find the common nodes which are shared among inputLinks
        for (unsigned i = 0; i < inputLinks.size() - 1; i ++)
        {
            for (unsigned j = i+1; j < inputLinks.size(); j ++)
            {
                vector<Handle> commonNodes;
                // get the common nodes in allNodesInEachLink[i] and allNodesInEachLink[j]
                std::set_intersection(allNodesInEachLink[i].begin(), allNodesInEachLink[i].end(),
                                      allNodesInEachLink[j].begin(), allNodesInEachLink[j].end(),
                                      std::back_inserter(commonNodes));

                if (commonNodes.size() > 0)
                    oneOfEachSeqShouldBeVars.push_back(commonNodes);

            }

        }
    }



    if ( (enable_filter_leaves_should_not_be_vars) || (enable_filter_node_types_should_not_be_vars) )
    {

        for (unsigned i = 0; i < inputLinks.size(); i ++)
        {
            for (Handle node : allNodesInEachLink[i])
            {

                // find leaves
                if (enable_filter_leaves_should_not_be_vars)
                {
                    bool is_leaf = true;

                    // try to find if this node exist in other links of inputLinks
                    for (unsigned j = 0; j < inputLinks.size(); j ++)
                    {
                        if (j == i)
                            continue;

                        if (allNodesInEachLink[j].find(node) != allNodesInEachLink[j].end())
                        {
                            is_leaf = false;
                            break;
                        }

                    }

                    if (is_leaf)
                        leaves.push_back(node);
                }

                // check if this node is in node_types_should_not_be_vars
                if (enable_filter_node_types_should_not_be_vars)
                {
                    Type t = _atomSpace->get_type(node);
                    for (Type noType : node_types_should_not_be_vars)
                    {
                        if (t == noType)
                        {
                            shouldNotBeVars.push_back(node);
                            break;
                        }
                    }
                }
            }
        }
    }

    return false;

}

// return true if the inputLinks are disconnected
// when the inputLinks are connected, the outputConnectedGroups has only one group, which is the same as inputLinks
bool PatternMiner::splitDisconnectedLinksIntoConnectedGroups(HandleSeq& inputLinks, HandleSeqSeq& outputConnectedGroups)
{
    if(inputLinks.size() < 2)
        return false;

    set<Handle> allNodesInEachLink[inputLinks.size()];
    for (unsigned int i = 0; i < inputLinks.size(); ++i)
    {
        extractAllVariableNodesInLink(inputLinks[i],allNodesInEachLink[i], atomSpace);
    }

    int i = -1;
    for (Handle link : inputLinks)
    {
        i ++;

        if (isInHandleSeqSeq(link, outputConnectedGroups))
            continue;

        // This link is not in outputConnectedGroups, which means none of its previous links connect to this link.
        // So, make a new group.
        HandleSeq newGroup;
        newGroup.push_back(link);

        // Only need to scan the links after this link
        for (unsigned int j = i+1; j < inputLinks.size(); j++)
        {
            for (Handle node : allNodesInEachLink[i])
            {
                if (allNodesInEachLink[j].find(node) != allNodesInEachLink[j].end())
                {
                    // they share same node -> they are connected.
                    newGroup.push_back(inputLinks[j]);
                    break;
                }
            }
        }

        outputConnectedGroups.push_back(newGroup);

    }

    return  (outputConnectedGroups.size() > 1);

}

double PatternMiner::calculateEntropyOfASubConnectedPattern(string& connectedSubPatternKey, HandleSeq& connectedSubPattern)
{
    // try to find if it has a correponding HtreeNode
    map<string, HTreeNode*>::iterator subPatternNodeIter = keyStrToHTreeNodeMap.find(connectedSubPatternKey);
    if (subPatternNodeIter != keyStrToHTreeNodeMap.end())
    {
        // it's in the H-Tree, add its entropy
        HTreeNode* subPatternNode = (HTreeNode*)subPatternNodeIter->second;
        // cout << "CalculateEntropy: Found in H-tree! h = log" << subPatternNode->count << " ";
        return log2(subPatternNode->count);
    }
    else
    {
        // can't find its HtreeNode, have to calculate its frequency again by calling pattern matcher
        // Todo: need to decide if add this missing HtreeNode into H-Tree or not

        HTreeNode* newHTreeNode = new HTreeNode();
        keyStrToHTreeNodeMap.insert(std::pair<string, HTreeNode*>(connectedSubPatternKey, newHTreeNode));
        newHTreeNode->pattern = connectedSubPattern;

        // Find All Instances in the original AtomSpace For this Pattern
        findAllInstancesForGivenPatternInNestedAtomSpace(newHTreeNode);
        // cout << "CalculateEntropy: Not found in H-tree! call pattern matcher again! h = log" << newHTreeNode->count << " ";

        int count = newHTreeNode->count;

        return log2(count);

    }
}


void PatternMiner::calculateInteractionInformation(HTreeNode* HNode)
{
    // this pattern has HandleSeq HNode->pattern.size() gram
    // the formula of interaction information I(XYZ..) =    sign*( H(X) + H(Y) + H(Z) + ...)
    //                                                   + -sign*( H(XY) + H(YZ) + H(XZ) + ...)
    //                                                   +  sign*( H(XYZ) ...)
    // H(X) is the entropy of X
    // Because in our sistuation, for each pattern X, we only care about how many instance it has, we don't record how many times each instance repeats.
    // Let C(X) as the count of instances for patten X, so each instance x for pattern X has an equal chance  1/C(X) of frequency.
    // Therefore, ,  H(X) =  (1/C(X))*log2(C(X))*1/C(X) = log2(C(X)). e.g. if a pattern appears 8 times in a corpus, its entropy is log2(8)

    // First, find all its subpatterns (its parent nodes in the HTree).
    // For the subpatterns those are mising in the HTree, need to call Pattern Matcher to find its frequency again.

//    std::cout << "=================Debug: calculateInteractionInformation for pattern: ====================\n";
//    for (Handle link : HNode->pattern)
//    {
//        std::cout << atomSpace->atom_as_string(link);
//    }

//    std::cout << "II = ";

    // Start from the last gram:
    int maxgram = HNode->pattern.size();
    int sign;
    if (maxgram%2)
        sign = 1;
    else
        sign = -1;

    double II = sign * log2(HNode->count);
//    std::cout << "H(curpattern) = log" << HNode->count << "="  << II << " sign=" << sign << std::endl;


    for (int gram = maxgram-1; gram > 0; gram --)
    {
         sign *= -1;
//         std::cout << "start subpatterns of gram = " << gram << std::endl;

         bool* indexes = new bool[maxgram];

         // generate the first combination
         for (int i = 0; i < gram; ++ i)
             indexes[i] = true;

         for (int i = gram; i < maxgram; ++ i)
             indexes[i] = false;

         while(true)
         {
             HandleSeq subPattern;
             for (int index = 0; index < maxgram; index ++)
             {
                 if (indexes[index])
                     subPattern.push_back(HNode->pattern[index]);
             }

             unsigned int unifiedLastLinkIndex;
             HandleSeq unifiedSubPattern = UnifyPatternOrder(subPattern, unifiedLastLinkIndex);
             string subPatternKey = unifiedPatternToKeyString(unifiedSubPattern);

//             std::cout<< "Subpattern: " << subPatternKey;

             // First check if this subpattern is disconnected. If it is disconnected, it won't exist in the H-Tree anyway.
             HandleSeqSeq splittedSubPattern;
             if (splitDisconnectedLinksIntoConnectedGroups(unifiedSubPattern, splittedSubPattern))
             {
//                 std::cout<< " is disconnected! splitted it into connected parts: \n" ;
                 // The splitted parts are disconnected, so they are independent. So the entroy = the sum of each part.
                 // e.g. if ABC is disconneted, and it's splitted into connected subgroups by splitDisconnectedLinksIntoConnectedGroups,
                 // for example: AC, B  then H(ABC) = H(AC) + H(B)
                 for (HandleSeq aConnectedSubPart : splittedSubPattern)
                 {
                     // Unify it again
                     unsigned int _unifiedLastLinkIndex;
                     HandleSeq unifiedConnectedSubPattern = UnifyPatternOrder(aConnectedSubPart, _unifiedLastLinkIndex);
                     string connectedSubPatternKey = unifiedPatternToKeyString(unifiedConnectedSubPattern);
//                     cout << "a splitted part: " << connectedSubPatternKey;
                     double h = calculateEntropyOfASubConnectedPattern(connectedSubPatternKey, unifiedConnectedSubPattern);
                     II += sign*h;
//                     cout << "sign="<<sign << " h =" << h << std::endl << std::endl;

                 }

             }
             else
             {
//                 std::cout<< " is connected! \n" ;
                 double h =calculateEntropyOfASubConnectedPattern(subPatternKey, unifiedSubPattern);
                 II += sign*h;
//                 cout << "sign="<<sign << " h =" << h << std::endl << std::endl;
             }


             if (isLastNElementsAllTrue(indexes, maxgram, gram))
                 break;

             // generate the next combination
             generateNextCombinationGroup(indexes, maxgram);
         }

         delete [] indexes;

    }

    HNode->interactionInformation = II;
//    std::cout<< "\n total II = " << II << "\n" ;

}

// try to use H-tree first
//void PatternMiner::calculateInteractionInformation(HTreeNode* HNode)
//{
//    // this pattern has HandleSeq HNode->pattern.size() gram
//    // the formula of interaction information I(XYZ..) =    sign*( H(X) + H(Y) + H(Z) + ...)
//    //                                                   + -sign*( H(XY) + H(YZ) + H(XZ) + ...)
//    //                                                   +  sign*( H(XYZ) ...)
//    // H(X) is the entropy of X
//    // Because in our sistuation, for each pattern X, we only care about how many instance it has, we don't record how many times each instance repeats.
//    // Let C(X) as the count of instances for patten X, so each instance x for pattern X has an equal chance  1/C(X) of frequency.
//    // Therefore, ,  H(X) =  (1/C(X))*log2(C(X))*1/C(X) = log2(C(X)). e.g. if a pattern appears 8 times in a corpus, its entropy is log2(8)

//    // First, find all its subpatterns (its parent nodes in the HTree).
//    // For the subpatterns those are mising in the HTree, need to call Pattern Matcher to find its frequency again.

//    std::cout << "=================Debug: calculateInteractionInformation for pattern: ====================\n";
//    for (Handle link : HNode->pattern)
//    {
//        std::cout << atomSpace->atom_as_string(link);
//    }

//    std::cout << "II = ";

//    // Start from the last gram:
//    int maxgram = HNode->pattern.size();
//    double II = - log2(HNode->instances.size());
//    std::cout << "-H(curpattern) = log" << HNode->instances.size() << "="  << II  << std::endl;


//    set<HTreeNode*> lastlevelNodes;

//    lastlevelNodes.insert(HNode);

//    int sign = 1;
//    for (int gram = maxgram-1; gram > 0; gram --)
//    {
//         std::cout << "start subpatterns of gram = " << gram << std::endl;
//        // get how many subpatterns this gram should have
//        unsigned int combinNum = combinationCalculate(gram, maxgram);
//        set<HTreeNode*> curlevelNodes;
//        set<string> keystrings;

//        for (HTreeNode* curNode : lastlevelNodes)
//        {
//            for (HTreeNode* pNode : curNode->parentLinks)
//            {
//                if (curlevelNodes.find(pNode) == curlevelNodes.end())
//                {
//                    std::cout << "H(subpattern): \n";
//                    for (Handle link : pNode->pattern)
//                    {
//                        std::cout << atomSpace->atom_as_string(link);
//                    }

//                    std::cout << "= " <<  sign << "*" << "log" << pNode->instances.size() << "=" << sign*log2(pNode->instances.size()) << "\n";

//                    II += sign*log2(pNode->instances.size());
//                    curlevelNodes.insert(pNode);
//                    keystrings.insert(unifiedPatternToKeyString(pNode->pattern));
//                }
//            }
//        }

//        if (curlevelNodes.size() < combinNum)
//        {
//             std::cout<<"Debug: calculateInteractionInformation:  missing sub patterns in the H-Tree, need to regenerate them." << std::endl;
//             bool* indexes = new bool[maxgram];

//             // generate the first combination
//             for (int i = 0; i < gram; ++ i)
//                 indexes[i] = true;

//             for (int i = gram; i < maxgram; ++ i)
//                 indexes[i] = false;

//             while(true)
//             {
//                 HandleSeq subPattern;
//                 for (int index = 0; index < maxgram; index ++)
//                 {
//                     if (indexes[index])
//                         subPattern.push_back(HNode->pattern[index]);
//                 }

//                 HandleSeq unifiedSubPattern = UnifyPatternOrder(subPattern);
//                 string subPatternKey = unifiedPatternToKeyString(unifiedSubPattern);

//                 std::cout<< "Subpattern: " << subPatternKey;

//                 if (keystrings.find(subPatternKey)== keystrings.end())
//                 {
//                     std::cout<< " not found above! \n" ;

//                     // a missing subpattern
//                     // First check if this subpattern is disconnected. If it is disconnected, it won't exist in the H-Tree anyway.
//                     HandleSeqSeq splittedSubPattern;
//                     if (splitDisconnectedLinksIntoConnectedGroups(unifiedSubPattern, splittedSubPattern))
//                     {
//                         std::cout<< " is disconnected! splitted it into connected parts: \n" ;
//                         // The splitted parts are disconnected, so they are independent. So the entroy = the sum of each part.
//                         // e.g. if ABC is disconneted, and it's splitted into connected subgroups by splitDisconnectedLinksIntoConnectedGroups,
//                         // for example: AC, B  then H(ABC) = H(AC) + H(B)
//                         for (HandleSeq aConnectedSubPart : splittedSubPattern)
//                         {
//                             // Unify it again
//                             HandleSeq unifiedConnectedSubPattern = UnifyPatternOrder(aConnectedSubPart);
//                             string connectedSubPatternKey = unifiedPatternToKeyString(unifiedConnectedSubPattern);
//                             cout << "a splitted part: " << connectedSubPatternKey;
//                             double h = calculateEntropyOfASubConnectedPattern(connectedSubPatternKey, unifiedConnectedSubPattern);
//                             II += sign*h;
//                             cout << "sign="<<sign << " h =" << h << std::endl << std::endl;

//                         }

//                     }
//                     else
//                     {
//                         std::cout<< " is connected! \n" ;
//                         double h =calculateEntropyOfASubConnectedPattern(subPatternKey, unifiedSubPattern);
//                         II += sign*h;
//                         cout << "sign="<<sign << " h =" << h << std::endl << std::endl;
//                     }

//                 }
//                 else
//                 {
//                     std::cout<< " already calculated above! skip! \n\n" ;
//                 }


//                 if (isLastNElementsAllTrue(indexes, maxgram, gram))
//                     break;

//                 // generate the next combination
//                 generateNextCombinationGroup(indexes, maxgram);
//             }
//        }


//        lastlevelNodes.clear();
//        lastlevelNodes = curlevelNodes;
//        sign *= -1;
//    }

//    HNode->interactionInformation = II;
//    std::cout<< "\n total II = " << II << "\n" ;

//}

unsigned int PatternMiner::getCountOfAConnectedPattern(string& connectedPatternKey, HandleSeq& connectedPattern)
{
    uniqueKeyLock.lock();
    // try to find if it has a correponding HtreeNode
    map<string, HTreeNode*>::iterator patternNodeIter = keyStrToHTreeNodeMap.find(connectedPatternKey);

    if (patternNodeIter != keyStrToHTreeNodeMap.end())
    {
        uniqueKeyLock.unlock();

        HTreeNode* patternNode = (HTreeNode*)patternNodeIter->second;

        return patternNode->count;
    }
    else
    {
        // can't find its HtreeNode, have to calculate its frequency again by calling pattern matcher
        // Todo: need to decide if add this missing HtreeNode into H-Tree or not

        // cout << "Exception: can't find a subpattern: \n" << connectedPatternKey << std::endl;
        if (run_as_central_server)
        {
            uniqueKeyLock.unlock();

            return 0;
        }
        else
        {

            HTreeNode* newHTreeNode = new HTreeNode();
            keyStrToHTreeNodeMap.insert(std::pair<string, HTreeNode*>(connectedPatternKey, newHTreeNode));
            uniqueKeyLock.unlock();

            newHTreeNode->pattern = connectedPattern;

            // Find All Instances in the original AtomSpace For this Pattern
            findAllInstancesForGivenPatternInNestedAtomSpace(newHTreeNode);
    //        cout << "Not found in H-tree! call pattern matcher again! count = " << newHTreeNode->count << std::endl;
            return newHTreeNode->count;
        }

    }


}

bool PatternMiner::isALinkOneInstanceOfGivenPattern(Handle &instanceLink, Handle& patternLink, AtomSpace* instanceLinkAtomSpace)
{
    if (instanceLinkAtomSpace->get_type(instanceLink) != atomSpace->get_type(patternLink))
        return false;

    HandleSeq outComingsOfPattern = atomSpace->get_outgoing(patternLink);
    HandleSeq outComingsOfInstance = instanceLinkAtomSpace->get_outgoing(instanceLink);

    if (outComingsOfPattern.size() != outComingsOfInstance.size())
        return false;

    for(unsigned int i = 0; i < outComingsOfPattern.size(); i ++)
    {
        bool pis_link = atomSpace->is_link(outComingsOfPattern[i]);
        bool iis_link = instanceLinkAtomSpace->is_link(outComingsOfInstance[i]);

        if (pis_link && iis_link)
        {
            if (isALinkOneInstanceOfGivenPattern(outComingsOfInstance[i], outComingsOfPattern[i], instanceLinkAtomSpace))
                continue;
            else
                return false;
        }
        else if ( (!pis_link)&&(!iis_link) )
        {
            // they are both nodes

            // If this node is a variable, skip it
            if (atomSpace->get_type(outComingsOfPattern[i]) == VARIABLE_NODE)
                continue;
            else
            {
                if (outComingsOfInstance[i] == outComingsOfPattern[i])
                    continue;
                else
                    return false;
            }
        }
        else
            return false; // this two atoms are of different type
    }

    return true;



}

void PatternMiner::reNameNodesForALink(Handle& inputLink, Handle& nodeToBeRenamed, Handle& newNamedNode,HandleSeq& renameOutgoingLinks,
                                       AtomSpace* _fromAtomSpace, AtomSpace* _toAtomSpace)
{
    HandleSeq outgoingLinks = _fromAtomSpace->get_outgoing(inputLink);

    for (Handle h : outgoingLinks)
    {

        if (_fromAtomSpace->is_node(h))
        {
           if (h == nodeToBeRenamed)
           {
               renameOutgoingLinks.push_back(newNamedNode);
           }
           else
           {
               // it's a not the node to be renamed, just add it
               renameOutgoingLinks.push_back(h);
           }
        }
        else
        {
             HandleSeq _renameOutgoingLinks;
             reNameNodesForALink(h, nodeToBeRenamed, newNamedNode, _renameOutgoingLinks, _fromAtomSpace, _toAtomSpace);
             Handle reLink = _toAtomSpace->add_link(_fromAtomSpace->get_type(h), _renameOutgoingLinks);
             reLink->merge(_fromAtomSpace->get_TV(h));
             renameOutgoingLinks.push_back(reLink);
        }

    }

}

// the return links is in Pattern mining AtomSpace
// toBeExtendedLink is the link contains leaf in the pattern
// varNode is the variableNode of leaf
void PatternMiner::getOneMoreGramExtendedLinksFromGivenLeaf(Handle& toBeExtendedLink, Handle& leaf, Handle& varNode,
                                                            HandleSeq& outPutExtendedPatternLinks, AtomSpace* _fromAtomSpace)
{
    HandleSeq incomings = _fromAtomSpace->get_incoming(leaf);

    for (Handle incomingHandle : incomings)
    {
        Handle extendedHandle;
        // if this atom is a igonred type, get its first parent that is not in the igonred types
        if (isIgnoredType (_fromAtomSpace->get_type(incomingHandle)) )
        {
            extendedHandle = getFirstNonIgnoredIncomingLink(_fromAtomSpace, incomingHandle);
            if (extendedHandle == Handle::UNDEFINED)
                continue;
        }
        else
            extendedHandle = incomingHandle;

        // skip this extendedHandle if it's one instance of toBeExtendedLink
        if (isALinkOneInstanceOfGivenPattern(extendedHandle, toBeExtendedLink, _fromAtomSpace))
            continue;

        // Rebind this extendedHandle into Pattern Mining Atomspace
        HandleSeq renameOutgoingLinks;
        reNameNodesForALink(extendedHandle, leaf, varNode, renameOutgoingLinks, _fromAtomSpace, atomSpace);
        Handle reLink = atomSpace->add_link(_fromAtomSpace->get_type(extendedHandle), renameOutgoingLinks);
        reLink->merge(_fromAtomSpace->get_TV(extendedHandle));
        outPutExtendedPatternLinks.push_back(reLink);
    }

}

// make sure only input 2~4 gram patterns, calculate nSurprisingness_I and nSurprisingness_II
void PatternMiner::calculateSurprisingness( HTreeNode* HNode, AtomSpace *_fromAtomSpace)
{

//    // debug
//    if (HNode->nI_Surprisingness != 0 || HNode->nII_Surprisingness != 0)
//        std::cout << "Exception: This pattern has been calculateSurprisingness before!\n";

    if (HNode->count == 0)
        HNode->count = 1;

    if (HNode->count < 2)
    {

        HNode->nII_Surprisingness = 0.0f;
        HNode->nI_Surprisingness = 0.0f;
        return;
    }

//    std::cout << "=================Debug: calculate I_Surprisingness for pattern: ====================\n";
//    for (Handle link : HNode->pattern)
//    {
//        std::cout << _fromAtomSpace->atom_as_string(link);
//    }
//     std::cout << "count of this pattern = " << HNode->count << std::endl;
//     std::cout << std::endl;

    unsigned int gram = HNode->pattern.size();
    // get the predefined combination:
    // vector<vector<vector<unsigned int>>>
    // int comcount = 0;

    float p = ((float)HNode->count)/atomspaceSizeFloat;
    float min_diff = 999999999.9f;
    // cout << "For this pattern itself: p = " <<  HNode->count << " / " <<  (int)atomspaceSizeFloat << " = " << p << std::endl;

    for (vector<vector<unsigned int>>&  oneCombin : components_ngram[gram-2])
    {
        int com_i = 0;
        // std::cout <<" -----Combination " << comcount++ << "-----" << std::endl;
        float total_p = 1.0f;

        bool containsComponentDisconnected = false;
        bool subComponentNotFound = false;

        for (vector<unsigned int>& oneComponent : oneCombin)
        {
            HandleSeq subPattern;
            for (unsigned int index : oneComponent)
            {
                subPattern.push_back(HNode->pattern[index]);
            }

            unsigned int unifiedLastLinkIndex;
            HandleSeq unifiedSubPattern = UnifyPatternOrder(subPattern, unifiedLastLinkIndex);
            string subPatternKey = unifiedPatternToKeyString(unifiedSubPattern);

            // std::cout<< "Subpattern: " << subPatternKey;

            // First check if this subpattern is disconnected. If it is disconnected, it won't exist in the H-Tree anyway.
            HandleSeqSeq splittedSubPattern;
            if (splitDisconnectedLinksIntoConnectedGroups(unifiedSubPattern, splittedSubPattern))
            {
                // std::cout<< " is disconnected! skip it \n" ;
                containsComponentDisconnected = true;
                break;
            }
            else
            {
                // std::cout<< " is connected!" ;
                unsigned int component_count = getCountOfAConnectedPattern(subPatternKey, unifiedSubPattern);

                if (component_count == 0)
                {
                    // one of the subcomponents is missing, skip this combination
                    subComponentNotFound = true;
                    break;
                }

                // cout << ", count = " << component_count;
                float p_i = ((float)(component_count)) / atomspaceSizeFloat;

                // cout << ", p = " << component_count  << " / " << (int)atomspaceSizeFloat << " = " << p_i << std::endl;
                total_p *= p_i;
                // std::cout << std::endl;
            }

            com_i ++;

        }

        if (containsComponentDisconnected || subComponentNotFound)
            continue;


        // cout << "\n ---- total_p = " << total_p << " ----\n" ;

        float diff = total_p - p;
        if (diff < 0)
            diff = - diff;

        // cout << "diff  = total_p - p " << diff << " \n" ;

        diff = diff / total_p;

        if (diff < min_diff)
            min_diff = diff;


        // cout << "diff / total_p = " << diff << " \n" ;

    }


    HNode->nI_Surprisingness = min_diff;

    // debug:
//    cout << "nI_Surprisingness = " << HNode->nI_Surprisingness  << std::endl;

    if (gram == MAX_GRAM ) // can't calculate II_Surprisingness for MAX_GRAM patterns, becasue it required gram +1 patterns
        return;

//    std::cout << "=================Debug: calculate II_Surprisingness for pattern: ====================\n";

    // II_Surprisingness is to evaluate how easily the frequency of this pattern can be infered from any of  its superpatterns
    // for all its super patterns
    if (HNode->superPatternRelations.size() == 0)
    {
        HNode->nII_Surprisingness  = 0.000000000f;
        // debug:
//        cout << "This node has no super patterns, give it Surprisingness_II value: 0.0 \n";
    }
    else
    {

        float minSurprisingness_II = 999999999.9f;
        vector<ExtendRelation>::iterator oneSuperRelationIt;
        for(oneSuperRelationIt = HNode->superPatternRelations.begin();  oneSuperRelationIt != HNode->superPatternRelations.end(); ++ oneSuperRelationIt)
        {
            ExtendRelation& curSuperRelation = *oneSuperRelationIt;

            // There are two types of super patterns: one is extended from a variable, one is extended from a const (turnt into a variable)
            // Only type two is the super pattern we are considering:

            // type one : extended from a variable,  the extended node itself is considered as a variable in the pattern A
            //            {
            //                // Ap is A's one supper pattern, E is the link pattern that extended
            //                // e.g.: M is the size of corpus
            //                // A:  ( var_1 is from CAR ) && ( var_1 is horror ) , P(A) = 100/M
            //                // A1: ( var_1 is from CAR ), A2: ( var_1 is horror )
            //                // Ap: ( var_1 is from CAR ) && ( var_1 is horror ) && ( var_1 is male ) , P(Ap) = 99/M
            //                // E:  ( var_1 is male )
            //                // Different from the super pattern type two bellow, E adds one more condition to var_1, so P(Ap) should be < or = P(A).
            //                // Surprisingness_II (A from Ap) =  min{|P(A) - P(Ap)*(P(Ai)/P(Ai&E))|} / P(A)
            //                // Actually in this example, both A and Ap are quite interesting


            // type two : extended from a const node , the const node is changed into a variable

            // Ap is A's one supper pattern, E is the link pattern that extended
            // e.g.: M is the size of corpus
            // A:  ( Lily eat var_1 ) && ( var_1 is vegetable ) , P(A) = 4/M
            // Ap: ( var_2 eat var_1 ) && ( var_1 is vegetable ) && ( var_2 is woman ) , P(Ap) = 20/M
            // E:  ( var_2 is woman ) , P(E) = 5/M
            // Surprisingness_II (A from Ap) =  |P(A) - P(Ap)*P(Lily)/P(E)| / P(A)
            // when the TruthValue of each atom is not taken into account, because every atom is unique, any P(Const atom) = 1/M , so that P(Lily) = 1/M
            // So: Surprisingness_II (A from Ap) =  |P(A) - P(Ap)/Count(E)| / P(A)

            // Note that becasue of unifying patern, the varible order in A, Ap, E can be different
            // HandleSeq& patternAp = curSuperRelation.extendedHTreeNode->pattern;

            float p_Ap = ((float )(curSuperRelation.extendedHTreeNode->count))/atomspaceSizeFloat;

            HandleSeq patternE;
            patternE.push_back(curSuperRelation.newExtendedLink);
            // unify patternE
            unsigned int unifiedLastLinkIndex;
            HandleSeq unifiedPatternE = UnifyPatternOrder(patternE, unifiedLastLinkIndex);
            string patternEKey = unifiedPatternToKeyString(unifiedPatternE, atomSpace);

            unsigned int patternE_count = getCountOfAConnectedPattern(patternEKey, unifiedPatternE);
            float p_ApDivByCountE = p_Ap / ( (float)(patternE_count) );

            float Surprisingness_II;
            if (p_ApDivByCountE > p)
                Surprisingness_II = p_ApDivByCountE - p;
            else
                Surprisingness_II = p - p_ApDivByCountE;

            if (Surprisingness_II < minSurprisingness_II)
                minSurprisingness_II = Surprisingness_II;

            // debug
//                cout << "For Super pattern: -------extended from a const----------------- " << std::endl;
//                cout << unifiedPatternToKeyString(curSuperRelation.extendedHTreeNode->pattern, atomSpace);
//                cout << "P(Ap) = " << p_Ap << std::endl;
//                cout << "The extended link pattern:  " << std::endl;
//                cout << patternEKey;
//                cout << "Count(E) = " << patternE_count << std::endl;
//                cout << "Surprisingness_II = |P(A) -P(Ap)/Count(E)| = " << Surprisingness_II << std::endl;




        }

//        // debug
//        cout << "Min Surprisingness_II  = " << minSurprisingness_II;

        if (HNode->superPatternRelations.size() > 0)
            HNode->nII_Surprisingness = minSurprisingness_II/p;
        else
        {
            HNode->nII_Surprisingness = -1.0f;
            // num_of_patterns_without_superpattern_cur_gram ++; // todo: need a lock
        }
    }



//    // debug:
//    cout << " nII_Surprisingness = Min Surprisingness_II / p = " << HNode->nII_Surprisingness  << std::endl;

//    std::cout << "=================Debug: end calculate II_Surprisingness ====================\n";

}

// in vector<vector<vector<unsigned int>>> the  <unsigned int> is the index in pattern HandleSeq : 0~n
void PatternMiner::generateComponentCombinations(string componentsStr, vector<vector<vector<unsigned int>>> &componentCombinations)
{
    // "0,12|1,02|2,01|0,1,2"

    vector<string> allCombinationsStrs = StringManipulator::split(componentsStr,"|");

    for (string oneCombinStr : allCombinationsStrs)
    {
        // "0,12"
        vector<vector<unsigned int>> oneCombin;

        vector<string> allComponentStrs = StringManipulator::split(oneCombinStr,",");
        for (string oneComponentStr : allComponentStrs)
        {
            vector<unsigned int> oneComponent;
            for(std::string::size_type i = 0; i < oneComponentStr.size(); ++i)
            {
                oneComponent.push_back((unsigned int)(oneComponentStr[i] - '0'));
            }

            oneCombin.push_back(oneComponent);

        }

        componentCombinations.push_back(oneCombin);
    }


}

PatternMiner::PatternMiner(AtomSpace* _originalAtomSpace): originalAtomSpace(_originalAtomSpace)
{
    htree = new HTree();
    atomSpace = new AtomSpace( _originalAtomSpace);

//    unsigned int system_thread_num  = std::thread::hardware_concurrency();

//    if (system_thread_num > 1)
//        THREAD_NUM = system_thread_num - 1;
//    else
//        THREAD_NUM = 1;

//     // use all the threads in this machine
//     THREAD_NUM = system_thread_num;

    THREAD_NUM = 1;

    run_as_distributed_worker = false;
    run_as_central_server = false;


    threads = new thread[THREAD_NUM];

    patternJsonArrays = new web::json::value[THREAD_NUM];

    int max_gram = config().get_int("Pattern_Max_Gram");
    MAX_GRAM = (unsigned int)max_gram;
    cur_gram = 0;

    ignoredTypes[0] = LIST_LINK;

    enable_Frequent_Pattern = config().get_bool("Enable_Frequent_Pattern");
    enable_Interesting_Pattern = config().get_bool("Enable_Interesting_Pattern");
    interestingness_Evaluation_method = config().get("Interestingness_Evaluation_method");

    assert(enable_Frequent_Pattern || enable_Interesting_Pattern);
    //The options are "Interaction_Information", "surprisingness"
    assert( (interestingness_Evaluation_method == "Interaction_Information") || (interestingness_Evaluation_method == "surprisingness") );

    enable_filter_leaves_should_not_be_vars = config().get_bool("enable_filter_leaves_should_not_be_vars");
    enable_filter_links_should_connect_by_vars = config().get_bool("enable_filter_links_should_connect_by_vars");
    enable_filter_node_types_should_not_be_vars =  config().get_bool("enable_filter_node_types_should_not_be_vars");
    enable_filter_not_inheritant_from_same_var = config().get_bool("enable_filter_not_inheritant_from_same_var");
    enable_filter_not_all_first_outgoing_const = config().get_bool("enable_filter_not_all_first_outgoing_const");
    enable_filter_not_same_var_from_same_predicate = config().get_bool("enable_filter_not_same_var_from_same_predicate");
    enable_filter_first_outgoing_evallink_should_be_var = config().get_bool("enable_filter_first_outgoing_evallink_should_be_var");
    if (enable_filter_node_types_should_not_be_vars)
    {
        string node_types_str = config().get("node_types_should_not_be_vars");
        node_types_str.erase(std::remove(node_types_str.begin(), node_types_str.end(), ' '), node_types_str.end());
        vector<string> typeStrs = StringManipulator::split(node_types_str,",");
        for (string typestr : typeStrs)
        {
            node_types_should_not_be_vars.push_back( classserver().getType(typestr) );
        }
    }

    // vector < vector<HTreeNode*> > patternsForGram
    for (unsigned int i = 0; i < MAX_GRAM; ++i)
    {
        vector<HTreeNode*> patternVector;
        patternsForGram.push_back(patternVector);

        vector<HTreeNode*> finalPatternVector;
        finalPatternsForGram.push_back(finalPatternVector);

    }

    // define (hard coding) all the possible subcomponent combinations for 2~4 gram patterns
    string gramNcomponents[3];
    // for 2 gram patterns [01], the only possible combination is [0][1]
    gramNcomponents[0] = "0,1";

    // for 3 gram patterns [012], the possible combinations are [0][12],[1][02],[2][01],[0][1][2]
    gramNcomponents[1] = "0,12|1,02|2,01|0,1,2";

    // for 4 gram patterns [0123], the possible combinations are:
    // [0][123],[1][023],[2][013], [3][012], [01][23],[02][13],[03][12],
    // [0][1][23],[0][2][13],[0][3][12],[1][2][03],[1][3][02],[2][3][01], [0][1][2][3]
    gramNcomponents[2] = "0,123|1,023|2,013|3,012|01,23|02,13|03,12|0,1,23|0,2,13|0,3,12|1,2,03|1,3,02|2,3,01|0,1,2,3";

    // generate vector<vector<vector<unsigned int>>> components_ngram[3] from above hard coding combinations for 2~4 grams

    int ngram = 0;
    for (string componentCombinsStr : gramNcomponents)
    {
        generateComponentCombinations(componentCombinsStr, this->components_ngram[ngram]);
        ngram ++;
    }

    // std::cout<<"Debug: PatternMiner init finished! " + toString(THREAD_NUM) + " threads used!" << std::endl;
}

PatternMiner::~PatternMiner()
{
    delete htree;
    delete atomSpace;
}

void PatternMiner::runPatternMiner(unsigned int _thresholdFrequency)
{

    thresholdFrequency = _thresholdFrequency;

    Pattern_mining_mode = config().get("Pattern_mining_mode"); // option: Breadth_First , Depth_First
    assert( (Pattern_mining_mode == "Breadth_First") || (Pattern_mining_mode == "Depth_First"));

    std::cout<<"Debug: PatternMining start! Max gram = " + toString(this->MAX_GRAM) << ", mode = " << Pattern_mining_mode << std::endl;

    int start_time = time(NULL);

    originalAtomSpace->get_handles_by_type(back_inserter(allLinks), (Type) LINK, true );

    allLinkNumber = (int)(allLinks.size());
    atomspaceSizeFloat = (float)(allLinkNumber);

    if (Pattern_mining_mode == "Breadth_First")
        runPatternMinerBreadthFirst();
    else
    {
        runPatternMinerDepthFirst();

        if (enable_Frequent_Pattern)
        {
            std::cout<<"Debug: PatternMiner:  done frequent pattern mining for 1 to "<< MAX_GRAM <<"gram patterns!\n";

            for(unsigned int gram = 1; gram <= MAX_GRAM; gram ++)
            {
                // sort by frequency
                std::sort((patternsForGram[gram-1]).begin(), (patternsForGram[gram-1]).end(),compareHTreeNodeByFrequency );

                // Finished mining gram patterns; output to file
                std::cout<<"gram = " + toString(gram) + ": " + toString((patternsForGram[gram-1]).size()) + " patterns found! ";

                OutPutFrequentPatternsToFile(gram);

                std::cout<< std::endl;
            }
        }


        if (enable_Interesting_Pattern)
        {
            for(cur_gram = 2; cur_gram <= MAX_GRAM; cur_gram ++)
            {

                cout << "\nCalculating interestingness for " << cur_gram << " gram patterns by evaluating " << interestingness_Evaluation_method << std::endl;
                cur_index = -1;
                threads = new thread[THREAD_NUM];
                num_of_patterns_without_superpattern_cur_gram = 0;

                for (unsigned int i = 0; i < THREAD_NUM; ++ i)
                {
                    threads[i] = std::thread([this]{this->evaluateInterestingnessTask();}); // using C++11 lambda-expression
                }

                for (unsigned int i = 0; i < THREAD_NUM; ++ i)
                {
                    threads[i].join();
                }

                delete [] threads;

                std::cout<<"Debug: PatternMiner:  done (gram = " + toString(cur_gram) + ") interestingness evaluation!" + toString((patternsForGram[cur_gram-1]).size()) + " patterns found! ";
                std::cout<<"Outputting to file ... ";

                if (interestingness_Evaluation_method == "Interaction_Information")
                {
                    // sort by interaction information
                    std::sort((patternsForGram[cur_gram-1]).begin(), (patternsForGram[cur_gram-1]).end(),compareHTreeNodeByInteractionInformation);
                    OutPutInterestingPatternsToFile(patternsForGram[cur_gram-1], cur_gram);
                }
                else if (interestingness_Evaluation_method == "surprisingness")
                {
                    // sort by surprisingness_I first
                    std::sort((patternsForGram[cur_gram-1]).begin(), (patternsForGram[cur_gram-1]).end(),compareHTreeNodeBySurprisingness_I);
                    OutPutInterestingPatternsToFile(patternsForGram[cur_gram-1], cur_gram,1);

                    if (cur_gram == MAX_GRAM)
                        break;

                    vector<HTreeNode*> curGramPatterns = patternsForGram[cur_gram-1];


                    // and then sort by surprisingness_II
                    std::sort(curGramPatterns.begin(), curGramPatterns.end(),compareHTreeNodeBySurprisingness_II);
                    OutPutInterestingPatternsToFile(curGramPatterns,cur_gram,2);

                    OutPutStaticsToCsvFile(cur_gram);

                    // Get the min threshold of surprisingness_II
                    int threshold_index_II;
                    threshold_index_II = SURPRISINGNESS_II_TOP_THRESHOLD * (float)(curGramPatterns.size() - num_of_patterns_without_superpattern_cur_gram);
                    int looptimes = 0;
                    while (true)
                    {

                        surprisingness_II_threshold = (curGramPatterns[threshold_index_II])->nII_Surprisingness;
                        if (surprisingness_II_threshold <= 0.00000f)
                        {
                            if (++ looptimes > 8)
                            {
                                surprisingness_II_threshold = 0.00000f;
                                break;
                            }

                            threshold_index_II = ((float)threshold_index_II) * SURPRISINGNESS_II_TOP_THRESHOLD;
                        }
                        else
                            break;
                    }


                    cout<< "surprisingness_II_threshold for " << cur_gram << " gram = "<< surprisingness_II_threshold;

                    // go through the top N patterns of surprisingness_I, pick the patterns with surprisingness_II higher than threshold
                    int threshold_index_I = SURPRISINGNESS_I_TOP_THRESHOLD * (float)(curGramPatterns.size());
                    for (int p = 0; p <= threshold_index_I; p ++)
                    {
                        HTreeNode* pNode = (patternsForGram[cur_gram-1])[p];

                        // for patterns have no superpatterns, nII_Surprisingness == -1.0, which should be taken into account
                        if ( (pNode->nII_Surprisingness < 0 ) || (pNode->nII_Surprisingness > surprisingness_II_threshold ) )
                            finalPatternsForGram[cur_gram-1].push_back(pNode);
                    }

                    // OutPutLowFrequencyHighSurprisingnessPatternsToFile(patternsForGram[cur_gram-1], cur_gram);

                    // OutPutHighFrequencyHighSurprisingnessPatternsToFile(patternsForGram[cur_gram-1], cur_gram,  15);

                    // OutPutHighSurprisingILowSurprisingnessIIPatternsToFile(patternsForGram[cur_gram-1], cur_gram, 100.0f, 0.51f);

                    // sort by frequency
                    std::sort((finalPatternsForGram[cur_gram-1]).begin(), (finalPatternsForGram[cur_gram-1]).end(),compareHTreeNodeByFrequency );

                    OutPutFinalPatternsToFile(cur_gram);

                }

                std::cout<< std::endl;
            }
        }
    }

    int end_time = time(NULL);
    printf("Pattern Mining Finish one round! Total time: %d seconds. \n", end_time - start_time);
    std::cout<< THREAD_NUM << " threads used. \n";
    std::cout<<"Corpus size: "<< allLinkNumber << " links in total. \n";

    std::cout << "Pattern Miner application quited!" << std::endl;
    std::exit(EXIT_SUCCESS);

//   testPatternMatcher2();

//   selectSubsetFromCorpus();

}

void PatternMiner::evaluateInterestingnessTask()
{

    while(true)
    {
        readNextPatternLock.lock();
        cur_index ++;

        if ((unsigned int)cur_index < (patternsForGram[cur_gram-1]).size())
        {
            cout<< "\r" << (float)(cur_index)/(float)((patternsForGram[cur_gram-1]).size())*100.0f << + "% completed." ;
            std::cout.flush();
        }
        else
        {
            if ((unsigned int)cur_index == (patternsForGram[cur_gram-1]).size())
            {
                cout<< "\r100% completed." ;
                std::cout.flush();
            }

            readNextPatternLock.unlock();
            break;

        }

        HTreeNode* htreeNode = patternsForGram[cur_gram - 1][cur_index];

        readNextPatternLock.unlock();

        // evaluate the interestingness
        // Only effective when Enable_Interesting_Pattern is true. The options are "Interaction_Information", "surprisingness"
        if (interestingness_Evaluation_method == "Interaction_Information")
        {
           calculateInteractionInformation(htreeNode);
        }
        else if (interestingness_Evaluation_method == "surprisingness")
        {
           calculateSurprisingness(htreeNode, observingAtomSpace);
        }

    }
}

void PatternMiner::selectSubsetFromCorpus(vector<string>& topics, unsigned int gram)
{
    // select a subset for test topics from the huge ConceptNet corpus
    _selectSubsetFromCorpus(topics,gram);
}

std::string PatternMiner::Link2keyString(Handle& h, std::string indent, const AtomSpace *atomspace)
{
    if (atomspace == 0)
        atomspace = this->atomSpace;

    std::stringstream answer;
    std::string more_indent = indent + LINE_INDENTATION;

    answer << indent  << "(" << classserver().getTypeName(atomspace->get_type(h)) << " ";

    if (atomspace->is_node(h))
    {
        answer << atomspace->get_name(h);
    }

    answer << ")" << "\n";

    if (atomspace->is_link(h))
    {
        HandleSeq outgoings = atomspace->get_outgoing(h);
        for (Handle outgoing : outgoings)
            answer << Link2keyString(outgoing, more_indent, atomspace);
    }

    return answer.str();
}

void PatternMiner::testPatternMatcher1()
{
    originalAtomSpace->get_handles_by_type(back_inserter(allLinks), (Type) LINK, true );
    std::cout<<"Debug: PatternMiner total link numer = "  << allLinks.size() << std::endl;

//(BindLink (stv 1.000000 1.000000)
//  (ListLink (stv 1.000000 1.000000)
//    (VariableNode "$var_1") ; [66]
//    (VariableNode "$var_2") ; [265]
//    (VariableNode "$var_3") ; [673]
//    (VariableNode "$var_4") ; [729]
//  ) ; [734]
//  (ImplicationLink (stv 1.000000 1.000000)
//    (AndLink (stv 1.000000 1.000000)
//      (EvaluationLink (stv 1.000000 1.000000)
//        (VariableNode "$var_1") ; [66]
//        (ListLink (stv 1.000000 1.000000)
//          (ObjectNode "Bob") ; [13]
//          (VariableNode "$var_2") ; [265]
//        ) ; [331]
//      ) ; [332]
//      (EvaluationLink (stv 1.000000 1.000000)
//        (VariableNode "$var_1") ; [66]
//        (ListLink (stv 1.000000 1.000000)
//          (VariableNode "$var_3") ; [673]
//          (VariableNode "$var_2") ; [265]
//        ) ; [930]
//      ) ; [931]
//      (InheritanceLink (stv 1.000000 1.000000)
//        (VariableNode "$var_3") ; [673]
//        (VariableNode "$var_4") ; [729]
//      ) ; [3482]
//    ) ; [5611]
//    ...
    HandleSeq variableNodes, implicationLinkOutgoings, bindLinkOutgoings, patternToMatch;

    Handle varHandle1 = originalAtomSpace->add_node(opencog::VARIABLE_NODE,"$var_1" );
    Handle varHandle2 = originalAtomSpace->add_node(opencog::VARIABLE_NODE,"$var_2" );
    Handle varHandle3 = originalAtomSpace->add_node(opencog::VARIABLE_NODE,"$var_3" );
    Handle varHandle4 = originalAtomSpace->add_node(opencog::VARIABLE_NODE,"$var_4" );


    variableNodes.push_back(varHandle1);
    variableNodes.push_back(varHandle2);
    variableNodes.push_back(varHandle3);
    variableNodes.push_back(varHandle4);

    // The first EvaluationLink
    HandleSeq listlinkOutgoings1, evalLinkOutgoings1;
    Handle bobNode = originalAtomSpace->add_node(opencog::OBJECT_NODE, "Bob" );
    listlinkOutgoings1.push_back(bobNode);
    listlinkOutgoings1.push_back(varHandle2);
    Handle listlink1 = originalAtomSpace->add_link(LIST_LINK, listlinkOutgoings1);
    // XXX why do we need to set the TV ???
    listlink1->merge(TruthValue::TRUE_TV());
    evalLinkOutgoings1.push_back(varHandle1);
    evalLinkOutgoings1.push_back(listlink1);
    Handle evalLink1 = originalAtomSpace->add_link(EVALUATION_LINK, evalLinkOutgoings1);
    // XXX why do we need to set the TV ???
    evalLink1->merge(TruthValue::TRUE_TV());

    // The second EvaluationLink
    HandleSeq listlinkOutgoings2, evalLinkOutgoings2;
    listlinkOutgoings2.push_back(varHandle3);
    listlinkOutgoings2.push_back(varHandle2);
    Handle listlink2 = originalAtomSpace->add_link(LIST_LINK, listlinkOutgoings2);
    // XXX why do we need to set the TV ???
    listlink2->merge(TruthValue::TRUE_TV());
    evalLinkOutgoings2.push_back(varHandle1);
    evalLinkOutgoings2.push_back(listlink2);
    Handle evalLink2 = originalAtomSpace->add_link(EVALUATION_LINK, evalLinkOutgoings2);
    // XXX why do we need to set the TV ???
    evalLink2->merge(TruthValue::TRUE_TV());

    // The InheritanceLink
    HandleSeq inherOutgoings;
    inherOutgoings.push_back(varHandle3);
    inherOutgoings.push_back(varHandle4);
    Handle inherLink = originalAtomSpace->add_link(INHERITANCE_LINK, inherOutgoings);
    // XXX why do we need to set the TV ???
    inherLink->merge(TruthValue::TRUE_TV());

    patternToMatch.push_back(evalLink1);
    patternToMatch.push_back(evalLink2);
    patternToMatch.push_back(inherLink);

    Handle hAndLink = originalAtomSpace->add_link(AND_LINK, patternToMatch);
    // XXX why do we need to set the TV ???
    hAndLink->merge(TruthValue::TRUE_TV());

    implicationLinkOutgoings.push_back(hAndLink); // the pattern to match
    implicationLinkOutgoings.push_back(hAndLink); // the results to return

    Handle hImplicationLink = originalAtomSpace->add_link(IMPLICATION_LINK, implicationLinkOutgoings);
    // XXX why do we need to set the TV ???
    hImplicationLink->merge(TruthValue::TRUE_TV());

    // add variable atoms
    Handle hVariablesListLink = originalAtomSpace->add_link(LIST_LINK, variableNodes);
    // XXX why do we need to set the TV ???
    hVariablesListLink->merge(TruthValue::TRUE_TV());

    bindLinkOutgoings.push_back(hVariablesListLink);
    bindLinkOutgoings.push_back(hImplicationLink);
    Handle hBindLink = originalAtomSpace->add_link(BIND_LINK, bindLinkOutgoings);
    // XXX why do we need to set the TV ???
    hBindLink->merge(TruthValue::TRUE_TV());

    std::cout <<"Debug: PatternMiner::testPatternMatcher for pattern:" << std::endl
              << originalAtomSpace->atom_as_string(hBindLink).c_str() << std::endl;


    // Run pattern matcher
    Handle hResultListLink = bindlink(originalAtomSpace, hBindLink);

    // Get result
    // Note: Don't forget to remove the hResultListLink and BindLink
    HandleSeq resultSet = originalAtomSpace->get_outgoing(hResultListLink);

    std::cout << toString(resultSet.size())  << " instances found:" << std::endl ;

    //debug
    std::cout << originalAtomSpace->atom_as_string(hResultListLink) << std::endl  << std::endl;

    originalAtomSpace->remove_atom(hResultListLink);
    originalAtomSpace->remove_atom(hBindLink);


}

void PatternMiner::testPatternMatcher2()
{

//    (BindLink (stv 1.000000 1.000000)
//      (ListLink (stv 1.000000 1.000000)
//        (VariableNode "$var_1") ; [63]
//        (VariableNode "$var_2") ; [545]
//        (VariableNode "$var_3") ; [2003]
//      ) ; [4575]
//      (ImplicationLink (stv 1.000000 1.000000)
//        (AndLink (stv 1.000000 1.000000)
//          (EvaluationLink (stv 1.000000 1.000000)
//            (VariableNode "$var_1") ; [63]
//            (ListLink (stv 1.000000 1.000000)
//              (ObjectNode "LiMing") ; [15]
//              (ConceptNode "tea") ; [20]
//            ) ; [44]
//          ) ; [4569]
//          (EvaluationLink (stv 1.000000 1.000000)
//            (VariableNode "$var_1") ; [63]
//            (ListLink (stv 1.000000 1.000000)
//              (VariableNode "$var_2") ; [545]
//              (VariableNode "$var_3") ; [2003]
//            ) ; [4570]
//          ) ; [4571]
//          (InheritanceLink (stv 1.000000 1.000000)
//            (VariableNode "$var_2") ; [545]
//            (ConceptNode "human") ; [3]
//          ) ; [4572]
//        ) ; [4573]
//        (AndLink (stv 1.000000 1.000000)
//        .....the same as the pattern
//      ) ; [4574]
//    ) ; [4576]

    HandleSeq variableNodes, implicationLinkOutgoings, bindLinkOutgoings, patternToMatch, resultOutgoings;

    Handle varHandle1 = originalAtomSpace->add_node(opencog::VARIABLE_NODE,"$var_1" );
    Handle varHandle2 = originalAtomSpace->add_node(opencog::VARIABLE_NODE,"$var_2" );
    Handle varHandle3 = originalAtomSpace->add_node(opencog::VARIABLE_NODE,"$var_3" );

    variableNodes.push_back(varHandle1);
    variableNodes.push_back(varHandle2);
    variableNodes.push_back(varHandle3);

    // The first EvaluationLink
    HandleSeq listlinkOutgoings1, evalLinkOutgoings1;
    Handle LiMingNode = originalAtomSpace->add_node(opencog::OBJECT_NODE, "LiMing" );
    listlinkOutgoings1.push_back(LiMingNode);
    Handle teaNode = originalAtomSpace->add_node(opencog::CONCEPT_NODE, "tea" );
    listlinkOutgoings1.push_back(teaNode);
    Handle listlink1 = originalAtomSpace->add_link(LIST_LINK, listlinkOutgoings1);
    // XXX why do we need to set the TV ???
    listlink1->merge(TruthValue::TRUE_TV());
    evalLinkOutgoings1.push_back(varHandle1);
    evalLinkOutgoings1.push_back(listlink1);
    Handle evalLink1 = originalAtomSpace->add_link(EVALUATION_LINK, evalLinkOutgoings1);
    // XXX why do we need to set the TV ???
    evalLink1->merge(TruthValue::TRUE_TV());

    // The second EvaluationLink
    HandleSeq listlinkOutgoings2, evalLinkOutgoings2;
    listlinkOutgoings2.push_back(varHandle2);
    listlinkOutgoings2.push_back(varHandle3);
    Handle listlink2 = originalAtomSpace->add_link(LIST_LINK, listlinkOutgoings2);
    // XXX why do we need to set the TV ???
    listlink2->merge(TruthValue::TRUE_TV());
    evalLinkOutgoings2.push_back(varHandle1);
    evalLinkOutgoings2.push_back(listlink2);
    Handle evalLink2 = originalAtomSpace->add_link(EVALUATION_LINK, evalLinkOutgoings2);
    // XXX why do we need to set the TV ???
    evalLink2->merge(TruthValue::TRUE_TV());

    // The InheritanceLink
    HandleSeq inherOutgoings;
    Handle humanNode = originalAtomSpace->add_node(opencog::CONCEPT_NODE, "human" );
    inherOutgoings.push_back(varHandle2);
    inherOutgoings.push_back(humanNode);
    Handle inherLink = originalAtomSpace->add_link(INHERITANCE_LINK, inherOutgoings);
    // XXX why do we need to set the TV ???
    inherLink->merge(TruthValue::TRUE_TV());

    patternToMatch.push_back(evalLink1);
    patternToMatch.push_back(evalLink2);
    patternToMatch.push_back(inherLink);

    Handle hAndLink = originalAtomSpace->add_link(AND_LINK, patternToMatch);
    // XXX why do we need to set the TV ???
    hAndLink->merge(TruthValue::TRUE_TV());

    // add variable atoms
    Handle hVariablesListLink = originalAtomSpace->add_link(LIST_LINK, variableNodes);
    // XXX why do we need to set the TV ???
    hVariablesListLink->merge(TruthValue::TRUE_TV());

    resultOutgoings.push_back(hVariablesListLink);
    resultOutgoings.push_back(hAndLink);

    Handle hListLinkResult = originalAtomSpace->add_link(LIST_LINK, resultOutgoings);
    // XXX why do we need to set the TV ???
    hListLinkResult->merge(TruthValue::TRUE_TV());

    implicationLinkOutgoings.push_back(hAndLink); // the pattern to match
    implicationLinkOutgoings.push_back(hListLinkResult); // the results to return

    Handle hImplicationLink = originalAtomSpace->add_link(IMPLICATION_LINK, implicationLinkOutgoings);
    // XXX why do we need to set the TV ???
    hImplicationLink->merge(TruthValue::TRUE_TV());


    bindLinkOutgoings.push_back(hVariablesListLink);
    bindLinkOutgoings.push_back(hImplicationLink);
    Handle hBindLink = originalAtomSpace->add_link(BIND_LINK, bindLinkOutgoings);
    // XXX why do we need to set the TV ???
    hBindLink->merge(TruthValue::TRUE_TV());

    std::cout <<"Debug: PatternMiner::testPatternMatcher for pattern:" << std::endl
              << originalAtomSpace->atom_as_string(hBindLink).c_str() << std::endl;


    // Run pattern matcher
    Handle hResultListLink = bindlink(originalAtomSpace, hBindLink);

    // Get result
    // Note: Don't forget to remove the hResultListLink and BindLink
    HandleSeq resultSet = originalAtomSpace->get_outgoing(hResultListLink);

    std::cout << toString(resultSet.size())  << " instances found:" << std::endl ;

    //debug
    std::cout << originalAtomSpace->atom_as_string(hResultListLink) << std::endl  << std::endl;

    originalAtomSpace->remove_atom(hResultListLink);
    originalAtomSpace->remove_atom(hBindLink);


}

set<Handle> PatternMiner::_getAllNonIgnoredLinksForGivenNode(Handle keywordNode, set<Handle>& allSubsetLinks)
{
    set<Handle> newHandles;
    HandleSeq incomings = originalAtomSpace->get_incoming(keywordNode);

    for (Handle incomingHandle : incomings)
    {
        Handle newh = incomingHandle;

        // if this atom is a igonred type, get its first parent that is not in the igonred types
        if (isIgnoredType (originalAtomSpace->get_type(incomingHandle)) )
        {
            newh = getFirstNonIgnoredIncomingLink(originalAtomSpace, incomingHandle);

            if ((newh == Handle::UNDEFINED) || containIgnoredContent(newh ))
                continue;
        }

        if (allSubsetLinks.find(newh) == allSubsetLinks.end())
            newHandles.insert(newh);

    }

    return newHandles;
}

set<Handle> PatternMiner::_extendOneLinkForSubsetCorpus(set<Handle>& allNewLinksLastGram, set<Handle>& allSubsetLinks)
{
    set<Handle> allNewConnectedLinksThisGram;
    // only extend the links in allNewLinksLastGram. allNewLinksLastGram is a part of allSubsetLinks
    for (Handle link : allNewLinksLastGram)
    {
        // find all nodes in this link
        set<Handle> allNodes;
        extractAllNodesInLink(link, allNodes, originalAtomSpace);

        for (Handle neighborNode : allNodes)
        {
            if (originalAtomSpace->get_type(neighborNode) == PREDICATE_NODE)
                continue;

            string content = originalAtomSpace->get_name(neighborNode);
            if (isIgnoredContent(content))
                continue;

            set<Handle> newConnectedLinks;
            newConnectedLinks = _getAllNonIgnoredLinksForGivenNode(neighborNode, allSubsetLinks);
            allNewConnectedLinksThisGram.insert(newConnectedLinks.begin(),newConnectedLinks.end());
            allSubsetLinks.insert(newConnectedLinks.begin(),newConnectedLinks.end());
        }

    }

    return allNewConnectedLinksThisGram;
}

// must load the corpus before calling this function
void PatternMiner::_selectSubsetFromCorpus(vector<string>& subsetKeywords, unsigned int max_connection)
{
    std::cout << "\nSelecting a subset from loaded corpus in Atomspace for the following topics:" << std::endl ;
    set<Handle> allSubsetLinks;
    string topicsStr = "";

    for (string keyword : subsetKeywords)
    {
        std::cout << keyword << std::endl;
        Handle keywordNode = originalAtomSpace->add_node(opencog::CONCEPT_NODE,keyword);
        set<Handle> newConnectedLinks = _getAllNonIgnoredLinksForGivenNode(keywordNode, allSubsetLinks);

        allSubsetLinks.insert(newConnectedLinks.begin(), newConnectedLinks.end());
        topicsStr += "-";
        topicsStr += keyword;

    }

    unsigned int order = 0;
    set<Handle> allNewConnectedLinksThisGram = allSubsetLinks;

    while (order < max_connection)
    {
        allNewConnectedLinksThisGram = _extendOneLinkForSubsetCorpus(allNewConnectedLinksThisGram, allSubsetLinks);
        order ++;
    }

    ofstream subsetFile;

    string fileName = "SubSet" + topicsStr + ".scm";

    subsetFile.open(fileName.c_str());

    // write the first line to enable unicode
    subsetFile <<  "(setlocale LC_CTYPE \"\")" << std::endl ;

    for (Handle h : allSubsetLinks)
    {
        if (containIgnoredContent(h))
            continue;

        subsetFile << originalAtomSpace->atom_as_string(h);
    }

    subsetFile.close();

    std::cout << "\nDone! The subset has been written to file:  " << fileName << std::endl ;
}

bool PatternMiner::isIgnoredContent(string keyword)
{
    for (string ignoreWord : ignoreKeyWords)
    {
        if (keyword == ignoreWord)
            return true;
    }

    return false;
}

bool PatternMiner::containIgnoredContent(Handle link )
{
    string str = originalAtomSpace->atom_as_string(link);

    for (string ignoreWord : ignoreKeyWords)
    {
        string ignoreStr = "\"" + ignoreWord + "\"";
        if (str.find(ignoreStr) != std::string::npos)
            return true;
    }

    return false;
}
