// math_utils.hpp
// Sample C++ header for indexer testing

#ifndef MATH_UTILS_HPP
#define MATH_UTILS_HPP

#include <vector>
#include <cmath>

namespace math {

// Constants
constexpr double PI = 3.14159265358979323846;
constexpr double E = 2.71828182845904523536;

// Simple class for 2D vectors
class Vector2D {
public:
    double x;
    double y;

    Vector2D() : x(0), y(0) {}
    Vector2D(double x, double y) : x(x), y(y) {}

    double length() const {
        return std::sqrt(x * x + y * y);
    }

    Vector2D normalize() const {
        double len = length();
        if (len > 0) {
            return Vector2D(x / len, y / len);
        }
        return *this;
    }

    Vector2D operator+(const Vector2D& other) const {
        return Vector2D(x + other.x, y + other.y);
    }

    Vector2D operator-(const Vector2D& other) const {
        return Vector2D(x - other.x, y - other.y);
    }

    double dot(const Vector2D& other) const {
        return x * other.x + y * other.y;
    }
};

// Template function for clamping values
template<typename T>
T clamp(T value, T min, T max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

// Free functions
double degrees_to_radians(double degrees);
double radians_to_degrees(double radians);

// Statistics functions
double mean(const std::vector<double>& values);
double variance(const std::vector<double>& values);
double standard_deviation(const std::vector<double>& values);

} // namespace math

#endif // MATH_UTILS_HPP
