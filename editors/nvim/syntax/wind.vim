if exists("b:current_syntax")
  finish
endif

syn keyword windStorage const
syn keyword windType string
syn keyword windBuiltin cout
syn match windOperator "<<\|="
syn match windComment "//.*$" contains=@Spell
syn match windEscape contained "\\[ntr0\\"]"
syn match windBadEscape contained "\\[^ntr0\\"]"
syn region windString start=+"+ skip=+\\.+ end=+"+ end=+$+ contains=windEscape,windBadEscape,@Spell

hi def link windStorage StorageClass
hi def link windType Type
hi def link windBuiltin Function
hi def link windOperator Operator
hi def link windComment Comment
hi def link windString String
hi def link windEscape SpecialChar
hi def link windBadEscape Error

let b:current_syntax = "wind"
