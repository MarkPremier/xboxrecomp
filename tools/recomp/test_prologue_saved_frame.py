"""A classic prologue must save the caller's EBP, not a C indeterminate value."""
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator


def test_prologue_saves_inherited_frame_before_establishing_its_own():
    base = 0x10000
    # push ebp; mov ebp,esp; mov eax,[ebp]; pop ebp; ret
    # The return value exposes the saved incoming frame pointer itself.
    image = bytes.fromhex('558bec8b45005dc3')
    config._install([config.Section('.text', base, len(image), 0, len(image), True)],
                    entry_point=base, kernel_thunk_addr=base,
                    origin='prologue-saved-frame-test')
    db = {base: {'start': hex(base), 'end': base + len(image),
                 '_addr': base, 'size': len(image)}}
    code = FunctionTranslator(image, db).translate_function(base, db[base])
    assert code.index('ebp = g_ebp;') < code.index('PUSH32(esp, ebp);')
    assert code.index('PUSH32(esp, ebp);') < code.index('ebp = esp;')
    assert 'eax = MEM32(ebp);' in code


def test_frameless_caller_preserves_general_ebp_across_nested_calls():
    base = 0x10000
    # push ebp; mov ebp,ecx; call eax; pop ebp; ret
    image = bytes.fromhex('558be9ffd05dc3')
    config._install([config.Section('.text', base, len(image), 0, len(image), True)],
                    entry_point=base, kernel_thunk_addr=base,
                    origin='frameless-general-ebp-test')
    db = {base: {'start': hex(base), 'end': base + len(image),
                 '_addr': base, 'size': len(image)}}
    code = FunctionTranslator(image, db).translate_function(base, db[base])
    assert 'ebp = ecx;' in code
    assert 'g_ebp = ebp; g_seh_ebp = ebp; { uint32_t _icall_target' in code
    assert 'g_ebp = ebp; g_seh_ebp = ebp; esp += 4; return;' in code
