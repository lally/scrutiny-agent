// math_utils.cpp
// Sample C++ implementation for indexer testing

#include "math_utils.hpp"
#include <numeric>

namespace math {

double degrees_to_radians(double degrees) {
    return degrees * PI / 180.0;
}

double radians_to_degrees(double radians) {
    return radians * 180.0 / PI;
}

double mean(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    double sum = std::accumulate(values.begin(), values.end(), 0.0);
    return sum / static_cast<double>(values.size());
}

double variance(const std::vector<double>& values) {
    if (values.size() < 2) {
        return 0.0;
    }
    double m = mean(values);
    double sum = 0.0;
    for (const auto& v : values) {
        double diff = v - m;
        sum += diff * diff;
    }
    return sum / static_cast<double>(values.size() - 1);
}

double standard_deviation(const std::vector<double>& values) {
    return std::sqrt(variance(values));
}

} // namespace math

// Example usage
int main() {
    math::Vector2D v1(3.0, 4.0);
    math::Vector2D v2(1.0, 2.0);

    auto v3 = v1 + v2;
    double len = v1.length();
    auto normalized = v1.normalize();
    double dotProduct = v1.dot(v2);

    double angle = math::degrees_to_radians(45.0);
    double clamped = math::clamp(150, 0, 100);

    std::vector<double> data = {1.0, 2.0, 3.0, 4.0, 5.0};
    double avg = math::mean(data);
    double stddev = math::standard_deviation(data);

    return 0;
}
