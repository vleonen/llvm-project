package main

import "fmt"

func main() {
    n := 20
    result := fibonacci(n)
    fmt.Printf("Fibonacci(%d) = %d\n", n, result)
}

func fibonacci(n int) int {
    if n <= 1 {
        return n
    }

    // Use iterative approach for better performance
    a, b := 0, 1
    for i := 2; i <= n; i++ {
        a, b = b, a+b
    }
    return b
}