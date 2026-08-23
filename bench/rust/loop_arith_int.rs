// The same benchmark written the way anyone would write it in Rust: integers.
//
// bench/rust/loop_arith.rs uses f64 because that is what a CScript number is,
// and f64 `%` is a call to fmod. This is what the language actually gives you
// when the type is yours to choose, and it is the number that matters when the
// question is "how fast is Rust".
fn main() {
    let mut sum: i64 = 0;
    let mut i: i64 = 0;
    while i < 10_000_000 {
        sum += i % 7;
        i += 1;
    }
    println!("{}", sum);
}
