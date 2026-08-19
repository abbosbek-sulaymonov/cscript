// && and || return an operand, not a boolean, and short-circuit.
print true && "kept";
print false && "skipped";
print false || "fallback";
print "first" || "unused";
print 0 || null || "last";
print 1 && 2 && 3;
print null && "never";
