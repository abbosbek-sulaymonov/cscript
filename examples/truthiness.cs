// Six values are falsy: false, null, undefined, 0, NaN and "".
// Everything else is truthy.

print !false;      // true
print !0;          // true
print !"";         // true
print !null;       // true
print !undefined;  // true
print !"anything"; // false

// && and || return one of their operands, not a boolean, and they
// short-circuit — the right side is never evaluated when the left decides it.
print "value" || "unused";
print null || "fallback";
print "first" && "second";
print 0 && "never reached";
