# x87 precision control

The original test checks 24-bit significand rounding while retaining the x87
exponent range, including the Burnout draw-list equality case. The reference
test compares the optimized helper against the prior implementation for four
million binary64 bit patterns and values at float/double boundaries, under all
four host rounding modes. NaNs, infinities and signed zero are included.

The ordinary float-range path is forced inline and checks the magnitude with
one unsigned comparison. Unusual exponents retain the prior slow path; no
arithmetic precision or rounding mode is relaxed.

The extended-memory test checks x87 80-bit loads/stores, including the CRT's
pi/2 endpoint constant, NaN/infinity/signed zero, ties, underflow and overflow.
One million finite binary64 values round-trip through the 10-byte format under
all four host rounding modes. The interpreter still keeps arithmetic in its
existing binary64 stack; this fixes operand decoding, not 80-bit arithmetic.

```powershell
cmake -S tests/fp_precision -B build/fp_precision -A x64
cmake --build build/fp_precision --config Release
ctest --test-dir build/fp_precision -C Release --output-on-failure
```
