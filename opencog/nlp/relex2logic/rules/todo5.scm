; This rule is for "She wants to sing."  I hate these rules, which were
; here when I got here.  There are five of them. There could be fifty.
; There would need to be, to cover all cases.  Somebody randomly chose
; five sentence-types to enshrine in these rules.  See my comment in
; the old relex2logic rule-file for more of an explanation of the problem.
; (AN June 2015)

(define todo5
	(BindLink
		(VariableList
			(var-decl "$a-parse" "ParseNode")
			(var-decl "$subj" "WordInstanceNode")
			(var-decl "$subj2" "WordInstanceNode")
			(var-decl "$verb1" "WordInstanceNode")
			(var-decl "$verb2" "WordInstanceNode")
			(var-decl "$obj" "WordInstanceNode")
		)
		(AndLink
			(word-in-parse "$subj" "$a-parse")
			(word-in-parse "$verb1" "$a-parse")
			(word-in-parse "$verb2" "$a-parse")
			(dependency "_subj" "$verb1" "$subj")
			(AbsentLink
			(dependency "_subj" "$verb2" "$subj2")
			)
			(AbsentLink
			(dependency "_obj" "$verb2" "$obj")
			)
			(dependency "_to-do" "$verb1" "$verb2")
		)
		(ExecutionOutputLink
			(GroundedSchemaNode "scm: pre-todo5-rule")
			(ListLink
				(VariableNode "$subj")
				(VariableNode "$verb1")
				(VariableNode "$verb2")
			)
		)
	)
)

; This is function is not needed. It is added so as not to break the existing
; r2l pipeline.
(define (pre-todo5-rule subj verb1 verb2)
  (to-do-rule-5
	(cog-name (word-inst-get-lemma verb1)) (cog-name verb1)
	(cog-name (word-inst-get-lemma  verb2)) (cog-name verb2)
	(cog-name (word-inst-get-lemma  subj)) (cog-name subj)
  )
)
