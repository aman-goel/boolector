(set-logic QF_BV)
(declare-fun s () (_ BitVec 8))
(declare-fun t () (_ BitVec 8))
(assert (not (= (bvnor s t) (bvnot (bvor s t)))))
(check-sat)
(exit)
