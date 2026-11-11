import unittest
from .disasm import Instruction
from .lifter import Lifter


class ScaleLifterTest(unittest.TestCase):
    def test_scale_does_not_cast_an_unbounded_exponent(self):
        text = " ".join(Lifter().lift_instruction(Instruction(0, 2, "fscale", "", "")))
        self.assertIn("recomp_fscale(fp_top(), fp_st1())", text)
        self.assertNotIn("(int)fp_st1()", text)


if __name__ == "__main__":
    unittest.main()
