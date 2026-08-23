// bench/methods.cx.
//
// black_box on the receivers, because `a.dot(&b)` is loop-invariant and LLVM
// will otherwise fold three million iterations into one multiply at compile
// time — which measures the optimiser, not the language.
use std::hint::black_box;

struct Vec2 {
    x: f64,
    y: f64,
}

impl Vec2 {
    fn dot(&self, other: &Vec2) -> f64 {
        self.x * other.x + self.y * other.y
    }
}

fn main() {
    let a = Vec2 { x: 3.0, y: 4.0 };
    let b = Vec2 { x: 5.0, y: 6.0 };
    let mut total: f64 = 0.0;
    let mut i: f64 = 0.0;
    while i < 3_000_000.0 {
        total += black_box(&a).dot(black_box(&b));
        i += 1.0;
    }
    println!("{}", total);
}
