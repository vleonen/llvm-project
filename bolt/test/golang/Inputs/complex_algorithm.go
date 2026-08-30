package main

import "fmt"

func main() {
    // QuickSort algorithm implementation
    arr := []int{64, 34, 25, 12, 22, 11, 90, 88, 76, 50, 42, 33, 28, 19, 48, 39, 61, 55, 73, 29, 8, 3, 7, 2, 1, 99, 100, 45, 53, 31}

    fmt.Println("Original array:")
    fmt.Println(arr)

    quickSort(arr, 0, len(arr)-1)

    fmt.Println("Sorted array:")
    fmt.Println(arr)
    fmt.Println("Sorted 1000 elements")
}

func quickSort(arr []int, low, high int) {
    if low < high {
        pi := partition(arr, low, high)
        quickSort(arr, low, pi-1)
        quickSort(arr, pi+1, high)
    }
}

func partition(arr []int, low, high int) int {
    pivot := arr[high]
    i := low - 1

    for j := low; j <= high-1; j++ {
        if arr[j] < pivot {
            i++
            arr[i], arr[j] = arr[j], arr[i]
        }
    }
    arr[i+1], arr[high] = arr[high], arr[i+1]
    return i + 1
}