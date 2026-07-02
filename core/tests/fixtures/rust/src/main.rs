// main.rs
// Example usage of the geometry library

use geometry::{Point, Circle, Rectangle, clamp};

fn main() {
    let origin = Point::origin();
    let p = Point::new(3.0, 4.0);
    println!("Distance from origin: {}", origin.distance_to(&p));

    let circle = Circle::new(origin, 5.0);
    println!("Circle area: {}", circle.area());
    println!("Point in circle: {}", circle.contains(&p));

    let rect = Rectangle::from_dimensions(0.0, 0.0, 10.0, 5.0);
    println!("Rectangle area: {}", rect.area());

    let clamped = clamp(15.0, 0.0, 10.0);
    println!("Clamped value: {}", clamped);
}
