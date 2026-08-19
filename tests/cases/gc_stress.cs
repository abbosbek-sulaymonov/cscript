// Allocates many distinct strings so the collector has to run and the intern
// pool has to survive it. Values still in use must not be swept.
print "a" + "b" + "c" + "d" + "e";
print ("x" + 1) + ("y" + 2) + ("z" + 3);
print "keep" + "me";
print "keep" + "me";      // interning: same contents, same object
print ("long " + 1000000) + (" and " + 2000000);
print typeof ("t" + "u");
print "final";
