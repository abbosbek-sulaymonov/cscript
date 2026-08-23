// The same computation as bench/loop_arith.cx, in f64 because that is what a
// CScript number is. Using i64 here would be a different program.
fn main() {
    let mut sum: f64 = 0.0;
    let mut i: f64 = 0.0;
    while i < 10_000_000.0 {
        sum += i % 7.0;
        i += 1.0;
    }
    println!("{}", sum);
}
