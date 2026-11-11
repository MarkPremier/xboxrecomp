import unittest
from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block


class LogicalShiftTest(unittest.TestCase):
    def test_counts_are_masked_and_values_are_unsigned(self):
        for mnemonic in ("shl", "shr"):
            for register in ("al", "ax", "eax"):
                instruction = Instruction(0, 2, mnemonic, "", "", operands=[
                    Operand(type="reg", reg=register), Operand(type="reg", reg="cl")])
                lifter = Lifter()
                lifter.needs_cf = True
                text = " ".join(lifter.lift_instruction(instruction))
                self.assertIn("& 31u", text)
                self.assertIn("(uint32_t)(", text)
                if mnemonic == "shl" and register != "eax":
                    self.assertIn("?", text)

    def test_masked_zero_preserves_the_incoming_condition(self):
        compare = Instruction(0, 3, "cmp", "eax, 7", "", operands=[
            Operand(type="reg", reg="eax"), Operand(type="imm", imm=7)])
        shift = Instruction(3, 3, "shr", "edx, 32", "", operands=[
            Operand(type="reg", reg="edx"), Operand(type="imm", imm=32)])
        branch = Instruction(6, 2, "je", "0x10", "", jump_target=0x10)
        text = "\n".join(lift_basic_block(Lifter(), BasicBlock(
            start=0, instructions=[compare, shift, branch]))[0])
        self.assertIn("masked count zero", text)
        self.assertIn("CMP_EQ(_fa, _fb)", text)


if __name__ == "__main__":
    unittest.main()
