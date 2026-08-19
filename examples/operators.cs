// Arithmetic follows the usual precedence: unary, then * / %, then + -.
print 1 + 2 * 3;      // 7  — not 9
print (1 + 2) * 3;    // 9
print 10 % 3;         // 1
print 2 * 3 % 4;      // 2
print 7 / 2;          // 3.5 — division is always floating point

// Comparisons produce booleans.
print 1 < 2;
print 3 >= 3;

// Division by zero gives Infinity rather than an error, like JavaScript.
print 1 / 0;
print 0 / 0;          // NaN
