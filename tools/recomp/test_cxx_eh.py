"""C++ catch continuations: anchor at the try registration, longjmp back.

Serious Sam (Xbox, USA) sub_000190D0 links a try-block registration whose
handler thunk 0x002C372B is `mov eax, 0x3BDAB0; jmp __CxxFrameHandler`, and its
catch funclet resumes at 0x00019197 -- an address nothing in the function
jumps to, so it had no label at all. The CRT's _JumpToContinuation
(0x002B4625) then ended in a `jmp eax` into the middle of sub_000190D0, which
recompiled code cannot do, with the throwing frames still on the native stack.
RtlRaiseException meanwhile returned to the throw site as if nothing happened,
so every "file not found, use the default" path in the engine carried on with
the failure instead of taking its catch.

Run: py -3 -m pytest tools/recomp/test_cxx_eh.py
"""

from tools.recomp import cxx_eh
from tools.recomp.disasm import Instruction, Operand
from tools.recomp.lifter import Lifter

FUNC, END = 0x000190D0, 0x000191C0
THUNK, CONT = 0x002C372B, 0x00019197
LINK = 0x000190E3            # mov fs:[0], esp


def _push_imm(addr, value):
    insn = Instruction(addr, 5, "push", f"0x{value:x}", "68" + "00" * 4)
    insn.operands = [Operand(type="imm", imm=value)]
    return insn


def _link(addr=LINK):
    insn = Instruction(addr, 7, "mov", "dword ptr fs:[0], esp", "64892500000000")
    insn.operands = [Operand(type="mem", mem_seg="fs", mem_size=4),
                     Operand(type="reg", reg="esp")]
    return insn


def test_registration_with_try_blocks_is_an_anchor_site():
    insns = [_push_imm(0x000190D3, 0xFFFFFFFF), _push_imm(0x000190D5, THUNK),
             _link()]
    sites = cxx_eh.function_sites(insns, {THUNK: frozenset({CONT, 0x00500000})},
                                  FUNC, END)
    # Only continuations inside this function can be branched to from it.
    assert sites == {LINK: {CONT}}


def test_destructor_only_registration_is_not_an_anchor():
    # A thunk whose FuncInfo has no try blocks never reaches `frames`.
    insns = [_push_imm(0x000190D5, 0x002C3740), _link()]
    assert cxx_eh.function_sites(insns, {THUNK: frozenset({CONT})}, FUNC, END) == {}


def test_continuation_split_into_its_own_function_is_kept():
    # sub_000186EA: the analysis made the continuation a function entry.
    insns = [_push_imm(0x000186A0, 0x002C34D2), _link(0x000186A5)]
    frames = {0x002C34D2: frozenset({0x000186EA})}
    assert cxx_eh.function_sites(insns, frames, 0x00018690, 0x000186EA,
                                 entries={0x000186EA}) == {0x000186A5: {0x000186EA}}
    assert cxx_eh.function_sites(insns, frames, 0x00018690, 0x000186EA) == {}


def test_split_continuation_resumes_as_a_tail_call():
    lifter = Lifter(func_db={0x000186EA: {"name": "sub_000186EA"}})
    lifter.func_start, lifter.func_end = 0x00018690, 0x000186EA
    lifter.eh_anchor_sites = {0x000186A5: {0x000186EA}}
    text = "\n".join(lifter.lift_instruction(_link(0x000186A5)))
    assert ("if (g_eh_target == 0x000186EAu) { g_seh_ebp = ebp;"
            " sub_000186EA(); return; }") in text
    assert "goto loc_000186EA" not in text


def test_anchor_is_armed_after_the_registration_is_linked():
    lifter = Lifter()
    lifter.func_start, lifter.func_end = FUNC, END
    lifter.eh_anchor_sites = {LINK: {CONT}}
    lines = lifter.lift_instruction(_link())
    text = "\n".join(lines)

    store = text.index("MEM32(XBOX_FS_BASE) = esp;")
    anchor = text.index("setjmp(*recomp_eh_anchor(esp))")
    assert store < anchor
    assert "ebp = g_ebp;" in text
    # One goto per line, so the translator's dead-label check sees each one.
    assert f"    if (g_eh_target == 0x{CONT:08X}u) goto loc_{CONT:08X};" in lines
    assert "recomp_eh_lost(g_eh_target); return; }" in lines[-1]


def test_jump_to_continuation_becomes_native_continue():
    jmp = Instruction(0x002B464E, 2, "jmp", "eax", "ffe0")
    jmp.operands = [Operand(type="reg", reg="eax")]
    lifter = Lifter()
    lifter.func_start, lifter.func_end = 0x002B4625, 0x002B4655
    lifter.eh_continue_sites = {0x002B464E}
    lines = lifter.lift_instruction(jmp)

    assert lines[0].startswith("g_ebp = ebp; recomp_eh_continue(eax, ebx);")
    # The plain indirect jump stays behind it as the fallback.
    assert len(lines) > 1


def test_jump_to_continuation_pattern_is_the_vc7_epilogue():
    # mov esp,[ebx-4]; mov ebp,[ebp-4]; jmp eax -- 0x002B4648 in Serious Sam.
    assert cxx_eh.JUMP_TO_CONTINUATION == bytes.fromhex("8b63fc8b6dfcffe0")


# Serious Sam sub_00358B27: `mov eax, 0x2E831A; call __EH_prolog (0x2C2D18)`.
# The helper links the record, so the function has no `mov fs:[0], esp` of
# its own; its catch resumes at 0x00358CB0.
PROLOG, PROLOG_THUNK, PROLOG_CONT = 0x002C2D18, 0x002E831A, 0x00358CB0


def _mov_eax(addr, value):
    insn = Instruction(addr, 5, "mov", f"eax, 0x{value:x}", "b8" + "00" * 4)
    insn.operands = [Operand(type="reg", reg="eax"), Operand(type="imm", imm=value)]
    return insn


def _call(addr, target):
    insn = Instruction(addr, 5, "call", f"0x{target:x}", "e8" + "00" * 4)
    insn.operands = [Operand(type="imm", imm=target)]
    return insn


def test_eh_prolog_call_is_an_anchor_site():
    insns = [_mov_eax(0x00358B27, PROLOG_THUNK), _call(0x00358B2C, PROLOG)]
    frames = {PROLOG_THUNK: frozenset({PROLOG_CONT})}
    assert cxx_eh.function_sites(insns, frames, 0x00358B27, 0x00358CD5,
                                 prologs={PROLOG}) == {0x00358B2C: {PROLOG_CONT}}
    # Without knowing the helper, or with a thunk that has no try blocks, no site.
    assert cxx_eh.function_sites(insns, frames, 0x00358B27, 0x00358CD5) == {}
    assert cxx_eh.function_sites(insns, {}, 0x00358B27, 0x00358CD5, prologs={PROLOG}) == {}
    # Something between the mov and the call breaks the pattern.
    gap = [_mov_eax(0x00358B27, PROLOG_THUNK), _push_imm(0x00358B2C, 1), _call(0x00358B31, PROLOG)]
    assert cxx_eh.function_sites(gap, frames, 0x00358B27, 0x00358CD5, prologs={PROLOG}) == {}


def test_eh_prolog_pattern_finds_the_helper():
    image = b"\xcc" * 16 + cxx_eh.EH_PROLOG + b"\xcc" * 16
    assert image.find(cxx_eh.EH_PROLOG) == 16
    assert len(cxx_eh.EH_PROLOG) == 31     # 0x2C2D18..0x2C2D36 inclusive


def test_eh_prolog_anchor_is_armed_after_the_call_at_the_record():
    lifter = Lifter()
    lifter.func_start, lifter.func_end = 0x00358B27, 0x00358CD5
    lifter.eh_anchor_sites = {0x00358B2C: {PROLOG_CONT}}
    lines = lifter.lift_instruction(_call(0x00358B2C, PROLOG))
    text = "\n".join(lines)
    # The call (and, in a full translation, its ebp read-back) comes first.
    call = text.index("2C2D18")
    anchor = text.index("setjmp(*recomp_eh_anchor(ebp - 0xCu))")
    assert call < anchor
    assert f"if (g_eh_target == 0x{PROLOG_CONT:08X}u) goto loc_{PROLOG_CONT:08X};" in text
