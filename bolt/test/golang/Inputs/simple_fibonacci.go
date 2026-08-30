package main

import "fmt"

func main() {
    n := 10
    result := fibonacci(n)
    fmt.Printf("Fibonacci(%d) = %d\n", n, result)
}

func fibonacci(n int) int {
    if n <= 1 {
        return n
    }
    return fibonacci(n-1) + fibonacci(n-2)
}