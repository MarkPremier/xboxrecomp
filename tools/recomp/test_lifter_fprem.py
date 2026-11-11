"""A stale unordered compare must not trap a completed CRT remainder loop."""
import re
import unittest
from .disasm import Instruction
from .lifter import Lifter, _make_condition


class RemainderStatusTest(unittest.TestCase):
    def test_complete_remainders_clear_c2(self):
        for mnemonic, function in (("fprem", "fmod"), ("fprem1", "remainder")):
            lifted = "\n".join(Lifter().lift_instruction(Instruction(0, 2, mnemonic, "", "")))
            self.assertIn(f"fp_top() = {function}(fp_top(), fp_st1())", lifted)
            self.assertIn("g_fp_cc &= (uint16_t)~0x0400u", lifted)

    def test_sahf_snapshots_ah_and_preserves_other_flags(self):
        lifted = "\n".join(Lifter().lift_instruction(Instruction(0, 1, "sahf", "", "9e")))
        self.assertIn("g_eflags = (g_eflags & ~0xD5u) | (HI8(eax) & 0xD5u)", lifted)
        self.assertNotIn("g_fp_cmp", lifted)

    def test_sahf_branches_follow_all_ah_bit_patterns(self):
        checks = {"jp": lambda f: bool(f & 4), "jnp": lambda f: not f & 4,
                  "jb": lambda f: bool(f & 1), "jae": lambda f: not f & 1,
                  "je": lambda f: bool(f & 64), "jne": lambda f: not f & 64,
                  "ja": lambda f: not f & 65, "jbe": lambda f: bool(f & 65),
                  "js": lambda f: bool(f & 128), "jns": lambda f: not f & 128}
        for branch, expected in checks.items():
            condition = _make_condition(branch, "sahf", [])[0]
            self.assertNotIn("g_fp_cmp", condition)
            expression = re.sub(r"(0x[0-9A-Fa-f]+)u", r"\1", condition).replace("&&", " and ").replace("||", " or ")
            for ah in range(256):
                self.assertEqual(bool(eval(expression, {"__builtins__": {}}, {"g_eflags": ah})), bool(expected(ah)))


if __name__ == "__main__":
    unittest.main()
