// CScript keeps JavaScript's two equality operators.
//
//   ==   coerces both sides, then compares
//   ===  compares type first, so different types are never equal

print 1 == "1";        // true  — the string is coerced to a number
print 1 === "1";       // false — number and string are different types

print 0 == false;      // true  — false coerces to 0
print 0 === false;     // false

print null == undefined;   // true — they are loosely equal to each other only
print null === undefined;  // false

// typeof null is "object". That is a bug in the original JavaScript that can
// never be fixed without breaking the web, and CScript reproduces it.
print typeof null;
