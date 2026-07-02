// geometry.rs
// Sample Rust module for indexer testing

use std::f64::consts::PI;

/// A 2D point structure
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Point {
    pub x: f64,
    pub y: f64,
}

impl Point {
    /// Create a new point
    pub fn new(x: f64, y: f64) -> Self {
        Point { x, y }
    }

    /// Create a point at the origin
    pub fn origin() -> Self {
        Point { x: 0.0, y: 0.0 }
    }

    /// Calculate distance to another point
    pub fn distance_to(&self, other: &Point) -> f64 {
        let dx = self.x - other.x;
        let dy = self.y - other.y;
        (dx * dx + dy * dy).sqrt()
    }

    /// Translate the point by given offsets
    pub fn translate(&mut self, dx: f64, dy: f64) {
        self.x += dx;
        self.y += dy;
    }
}

/// A circle defined by center and radius
#[derive(Debug, Clone)]
pub struct Circle {
    pub center: Point,
    pub radius: f64,
}

impl Circle {
    /// Create a new circle
    pub fn new(center: Point, radius: f64) -> Self {
        Circle { center, radius }
    }

    /// Calculate the area of the circle
    pub fn area(&self) -> f64 {
        PI * self.radius * self.radius
    }

    /// Calculate the circumference
    pub fn circumference(&self) -> f64 {
        2.0 * PI * self.radius
    }

    /// Check if a point is inside the circle
    pub fn contains(&self, point: &Point) -> bool {
        self.center.distance_to(point) <= self.radius
    }
}

/// A rectangle defined by two corner points
#[derive(Debug, Clone)]
pub struct Rectangle {
    pub top_left: Point,
    pub bottom_right: Point,
}

impl Rectangle {
    /// Create a new rectangle from two points
    pub fn new(top_left: Point, bottom_right: Point) -> Self {
        Rectangle { top_left, bottom_right }
    }

    /// Create a rectangle from position and dimensions
    pub fn from_dimensions(x: f64, y: f64, width: f64, height: f64) -> Self {
        Rectangle {
            top_left: Point::new(x, y),
            bottom_right: Point::new(x + width, y + height),
        }
    }

    /// Calculate width
    pub fn width(&self) -> f64 {
        (self.bottom_right.x - self.top_left.x).abs()
    }

    /// Calculate height
    pub fn height(&self) -> f64 {
        (self.bottom_right.y - self.top_left.y).abs()
    }

    /// Calculate area
    pub fn area(&self) -> f64 {
        self.width() * self.height()
    }

    /// Calculate perimeter
    pub fn perimeter(&self) -> f64 {
        2.0 * (self.width() + self.height())
    }

    /// Check if a point is inside the rectangle
    pub fn contains(&self, point: &Point) -> bool {
        point.x >= self.top_left.x
            && point.x <= self.bottom_right.x
            && point.y >= self.top_left.y
            && point.y <= self.bottom_right.y
    }
}

/// Shape trait for polymorphic behavior
pub trait Shape {
    fn area(&self) -> f64;
    fn perimeter(&self) -> f64;
}

impl Shape for Circle {
    fn area(&self) -> f64 {
        self.area()
    }

    fn perimeter(&self) -> f64 {
        self.circumference()
    }
}

impl Shape for Rectangle {
    fn area(&self) -> f64 {
        self.area()
    }

    fn perimeter(&self) -> f64 {
        self.perimeter()
    }
}

/// Calculate total area of multiple shapes
pub fn total_area<T: Shape>(shapes: &[T]) -> f64 {
    shapes.iter().map(|s| s.area()).sum()
}

/// Linear interpolation between two values
pub fn lerp(a: f64, b: f64, t: f64) -> f64 {
    a + (b - a) * t
}

/// Clamp a value between min and max
pub fn clamp(value: f64, min: f64, max: f64) -> f64 {
    if value < min {
        min
    } else if value > max {
        max
    } else {
        value
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_point_distance() {
        let p1 = Point::new(0.0, 0.0);
        let p2 = Point::new(3.0, 4.0);
        assert!((p1.distance_to(&p2) - 5.0).abs() < 1e-10);
    }

    #[test]
    fn test_circle_area() {
        let c = Circle::new(Point::origin(), 1.0);
        assert!((c.area() - PI).abs() < 1e-10);
    }

    #[test]
    fn test_rectangle_area() {
        let r = Rectangle::from_dimensions(0.0, 0.0, 4.0, 3.0);
        assert!((r.area() - 12.0).abs() < 1e-10);
    }
}
