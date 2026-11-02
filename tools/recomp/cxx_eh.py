"""
MSVC x86 C++ exception handling: where catch blocks resume.

A function with a try block links an EH registration into fs:[0] whose
handler is a per-function thunk,

    mov eax, offset FuncInfo        B8 <FuncInfo>
    jmp __CxxFrameHandler           E9 <rel32>

and FuncInfo leads to its catch funclets. Each funclet ends by handing the
CRT the address to resume at,

    mov eax, offset $continuation
    ret

and the CRT's _JumpToContinuation reloads that function's esp and ebp and
jumps there:

    mov esp, [ebx-4]                8B 63 FC
    mov ebp, [ebp-4]                8B 6D FC
    jmp eax                         FF E0

A recompiled function cannot be entered in the middle, and the frames between
the throw and the catch are native C frames that must unwind. The lifter
therefore arms a native anchor where such a function links its registration
(recomp_eh_anchor), gives every continuation a label, and turns that jmp into
recomp_eh_continue, a longjmp back to the anchor. See xbox_memory_layout.c.

FuncInfo (magic 0x19930520..0x19930522):
    +0  magic   +4 maxState   +8 pUnwindMap   +12 nTryBlocks   +16 pTryBlockMap
TryBlockMapEntry (20 bytes):
    +0 tryLow   +4 tryHigh   +8 catchHigh   +12 nCatches   +16 pHandlerArray
HandlerType (16 bytes):
    +0 adjectives   +4 pType   +8 dispCatchObj   +12 addressOfHandler
"""

import re
import struct

from . import config

_THUNK = re.compile(rb"\xB8(....)\xE9(....)", re.S)
JUMP_TO_CONTINUATION = bytes.fromhex("8b63fc8b6dfcffe0")
_FUNCLET_SCAN_BYTES = 0x400


def _u32(xbe_data, va):
    off = config.va_to_file_offset(va)
    if off is None or off + 4 > len(xbe_data):
        return None
    return struct.unpack_from("<I", xbe_data, off)[0]


def _funclet_continuations(xbe_data, funclet):
    """The values a catch funclet loads into eax just before a ret."""
    off = config.va_to_file_offset(funclet)
    if off is None:
        return set()
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    found, last = set(), None
    for ins in md.disasm(xbe_data[off:off + _FUNCLET_SCAN_BYTES], funclet):
        if ins.mnemonic == "ret":
            if last is not None:
                found.add(last)
            break
        if ins.mnemonic == "jmp" and not ins.op_str.startswith("0x"):
            break
        # Only the mov immediately before the ret is the continuation.
        last = (int(ins.op_str[5:], 16)
                if ins.mnemonic == "mov" and ins.op_str.startswith("eax, 0x")
                else None)
    return found


def scan(xbe_data):
    """Return (frames, continue_sites).

    frames: {handler thunk VA: frozenset of continuation VAs}, only for
            functions that have try blocks.
    continue_sites: VAs of the `jmp eax` that ends _JumpToContinuation.
    """
    frames, continue_sites = {}, set()
    for name, va, size, raw in config.SECTIONS:
        if not config.is_code_address(va):
            continue
        seg = xbe_data[raw:raw + size]
        for m in _THUNK.finditer(seg):
            info = struct.unpack("<I", m.group(1))[0]
            magic = _u32(xbe_data, info)
            if magic is None or magic & 0xFFFFFFF0 != 0x19930520 or magic > 0x19930522:
                continue
            ntry, ptry = _u32(xbe_data, info + 12), _u32(xbe_data, info + 16)
            if not ntry or ptry is None or ntry > 0x1000:
                continue
            conts = set()
            for t in range(ntry):
                ncatch = _u32(xbe_data, ptry + t * 20 + 12) or 0
                handlers = _u32(xbe_data, ptry + t * 20 + 16)
                if handlers is None or ncatch > 0x100:
                    continue
                for c in range(ncatch):
                    funclet = _u32(xbe_data, handlers + c * 16 + 12)
                    if funclet and config.is_code_address(funclet):
                        conts |= _funclet_continuations(xbe_data, funclet)
            if conts:
                frames[va + m.start()] = frozenset(conts)
        p = seg.find(JUMP_TO_CONTINUATION)
        while p >= 0:
            continue_sites.add(va + p + 6)
            p = seg.find(JUMP_TO_CONTINUATION, p + 1)
    return frames, continue_sites


def function_sites(instructions, frames, start, end, entries=()):
    """Map each `mov fs:[0], esp` that links a try-block registration to the
    continuations its function can resume at: those inside [start, end), and
    those that are themselves function entries in `entries`. The analysis
    splits a function at a continuation often enough -- the funclet's
    `mov eax, offset $continuation` reads as a code pointer -- and resuming
    there is then a tail call, as falling into the next piece is."""
    sites, pending = {}, None
    for insn in instructions:
        if insn.mnemonic == "push" and len(insn.operands) == 1 \
                and insn.operands[0].type == "imm":
            if insn.operands[0].imm in frames:
                pending = insn.operands[0].imm
            continue
        if pending is not None and insn.mnemonic == "mov" \
                and "fs:" in insn.op_str.replace(" ", "") \
                and insn.op_str.replace(" ", "").endswith(",esp"):
            conts = {c for c in frames[pending]
                     if start <= c < end or c in entries}
            if conts:
                sites[insn.address] = conts
            pending = None
    return sites
