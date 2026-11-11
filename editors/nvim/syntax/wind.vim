if exists("b:current_syntax")
  finish
endif

syn keyword windStorage const
syn keyword windConditional if else
syn keyword windStatement goto return
syn keyword windOperator sizeof
syn keyword windBuiltin cout
syn keyword windBoolean true false
syn keyword windType void bool string char byte
syn keyword windType int i8 i16 i32 i64 i128
syn keyword windType uint u8 u16 u32 u64 u128 uintptr
syn keyword windType float f16 f32 f64 f128
syn keyword windType unsigned signed long short double
syn keyword windType int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t
syn keyword windType intptr_t uintptr_t ptrdiff_t ssize_t size_t

syn match windOperator "<<\|&&\|||\|++\|--\|[-+*/%=!<>]=\|[-+*/%=<>!]"
syn match windLabel "^\s*\zs\h\w*\ze:"
syn match windFunction "\h\w*\ze\s*("

syn match windNumber "\<\d[0-9_]*[uUlL]*\>"
syn match windNumber "\<0[xX][0-9a-fA-F_]\+[uUlL]*\>"
syn match windNumber "\<0[bB][01_]\+[uUlL]*\>"
syn match windNumber "\<0[oO][0-7_]\+[uUlL]*\>"
syn match windFloat "\<\d[0-9_]*\.\d[0-9_]*\%([eE][-+]\=\d\+\)\=[fFlL]\=\>"
syn match windFloat "\<\d[0-9_]*[eE][-+]\=\d\+[fFlL]\=\>"

syn match windEscape contained "\\[ntr0\\"']"
syn match windBadEscape contained "\\[^ntr0\\"']"
syn region windString start=+"+ skip=+\\.+ end=+"+ end=+$+ contains=windEscape,windBadEscape,@Spell
syn match windChar "'\%(\\.\|[^\\']\)'" contains=windEscape,windBadEscape

syn match windComment "//.*$" contains=@Spell
syn region windComment start="/\*" end="\*/" contains=@Spell
syn sync minlines=200

hi def link windStorage StorageClass
hi def link windConditional Conditional
hi def link windStatement Statement
hi def link windBuiltin Function
hi def link windFunction Function
hi def link windBoolean Boolean
hi def link windType Type
hi def link windOperator Operator
hi def link windLabel Label
hi def link windNumber Number
hi def link windFloat Float
hi def link windComment Comment
hi def link windString String
hi def link windChar Character
hi def link windEscape SpecialChar
hi def link windBadEscape Error

let b:current_syntax = "wind"
