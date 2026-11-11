"""Paired with Sam's native x87 reference and real CRT sine regression."""
import unittest
from .disasm import Instruction
from .lifter import Lifter


class TrigStatusTest(unittest.TestCase):
    def test_range_check_precedes_computing_or_pushing(self):
        for mnemonic in ("fsin", "fcos", "fsincos", "fptan"):
            text = " ".join(Lifter().lift_instruction(Instruction(0, 2, mnemonic, "", "")))
            self.assertTrue(text.startswith("if (recomp_trig_ready(fp_top()))"))
        tangent = " ".join(Lifter().lift_instruction(Instruction(0, 2, "fptan", "", "")))
        self.assertIn("fp_push(isnan(_a) ? _a : 1.0)", tangent)


if __name__ == "__main__":
    unittest.main()
