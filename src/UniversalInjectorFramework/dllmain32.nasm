bits 32
section .text

global _EntryPointHook
extern _Attach
extern _EntryPoint

_EntryPointHook:
    call _Attach
    jmp [_EntryPoint]
