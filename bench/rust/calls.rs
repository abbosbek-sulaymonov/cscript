// bench/calls.cx. `mix` is left for the optimiser to inline or not, exactly as
// CScript's compiler is left to answer the call or not — inlining it by hand
// would be measuring a different thing.
fn mix(a: f64, b: f64) -> f64 {
    let scaled = a * 3.0 + b * 7.0;
    scaled % 1000.0 - a / 2.0
}

fn main() {
    let mut total: f64 = 0.0;
    let mut i: f64 = 0.0;
    while i < 2_000_000.0 {
        total += mix(i, i + 1.0);
        i += 1.0;
    }
    println!("{}", total);
}
