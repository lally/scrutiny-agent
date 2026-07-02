# calculator.py
# Sample Python module for indexer testing

from typing import List, Optional, Union
from dataclasses import dataclass
from enum import Enum
import math


class Operation(Enum):
    """Supported calculator operations."""
    ADD = "add"
    SUBTRACT = "subtract"
    MULTIPLY = "multiply"
    DIVIDE = "divide"
    POWER = "power"


@dataclass
class CalculationResult:
    """Result of a calculation."""
    value: float
    operation: Operation
    operands: List[float]
    error: Optional[str] = None


class Calculator:
    """A simple calculator class with history tracking."""

    def __init__(self, precision: int = 10):
        self.precision = precision
        self.history: List[CalculationResult] = []
        self._last_result: Optional[float] = None

    @property
    def last_result(self) -> Optional[float]:
        """Get the last calculation result."""
        return self._last_result

    def add(self, a: float, b: float) -> float:
        """Add two numbers."""
        result = round(a + b, self.precision)
        self._record(Operation.ADD, [a, b], result)
        return result

    def subtract(self, a: float, b: float) -> float:
        """Subtract b from a."""
        result = round(a - b, self.precision)
        self._record(Operation.SUBTRACT, [a, b], result)
        return result

    def multiply(self, a: float, b: float) -> float:
        """Multiply two numbers."""
        result = round(a * b, self.precision)
        self._record(Operation.MULTIPLY, [a, b], result)
        return result

    def divide(self, a: float, b: float) -> float:
        """Divide a by b."""
        if b == 0:
            self._record(Operation.DIVIDE, [a, b], 0, "Division by zero")
            raise ValueError("Cannot divide by zero")
        result = round(a / b, self.precision)
        self._record(Operation.DIVIDE, [a, b], result)
        return result

    def power(self, base: float, exponent: float) -> float:
        """Raise base to exponent."""
        result = round(math.pow(base, exponent), self.precision)
        self._record(Operation.POWER, [base, exponent], result)
        return result

    def _record(
        self,
        op: Operation,
        operands: List[float],
        value: float,
        error: Optional[str] = None
    ) -> None:
        """Record a calculation in history."""
        self._last_result = value
        self.history.append(CalculationResult(
            value=value,
            operation=op,
            operands=operands,
            error=error
        ))

    def clear_history(self) -> None:
        """Clear calculation history."""
        self.history.clear()
        self._last_result = None

    def get_history_summary(self) -> str:
        """Get a summary of calculation history."""
        if not self.history:
            return "No calculations performed."
        return f"Total calculations: {len(self.history)}"


def factorial(n: int) -> int:
    """Calculate factorial of n."""
    if n < 0:
        raise ValueError("Factorial not defined for negative numbers")
    if n <= 1:
        return 1
    return n * factorial(n - 1)


def fibonacci(n: int) -> List[int]:
    """Generate first n Fibonacci numbers."""
    if n <= 0:
        return []
    if n == 1:
        return [0]

    sequence = [0, 1]
    for _ in range(2, n):
        sequence.append(sequence[-1] + sequence[-2])
    return sequence


def is_prime(n: int) -> bool:
    """Check if n is a prime number."""
    if n < 2:
        return False
    if n == 2:
        return True
    if n % 2 == 0:
        return False
    for i in range(3, int(math.sqrt(n)) + 1, 2):
        if n % i == 0:
            return False
    return True


# Example usage
if __name__ == "__main__":
    calc = Calculator(precision=6)

    result = calc.add(10, 5)
    result = calc.subtract(result, 3)
    result = calc.multiply(result, 2)
    result = calc.divide(result, 4)
    result = calc.power(result, 2)

    print(f"Final result: {calc.last_result}")
    print(calc.get_history_summary())

    print(f"Factorial of 5: {factorial(5)}")
    print(f"First 10 Fibonacci: {fibonacci(10)}")
    print(f"Is 17 prime? {is_prime(17)}")
