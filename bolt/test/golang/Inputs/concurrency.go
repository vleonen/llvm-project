package main

import "fmt"
import "sync"

func main() {
    numJobs := 20
    numWorkers := 4

    jobs := make(chan int, numJobs)
    results := make(chan string, numJobs)

    var wg sync.WaitGroup

    // Start workers
    for i := 0; i < numWorkers; i++ {
        wg.Add(1)
        go func(workerID int) {
            defer wg.Done()
            for job := range jobs {
                result := processJob(workerID, job)
                results <- result
            }
        }(i)
    }

    // Send jobs
    for j := 0; j < numJobs; j++ {
        jobs <- j
    }
    close(jobs)

    // Wait for all workers to complete
    go func() {
        wg.Wait()
        close(results)
    }()

    // Collect results
    var processedCount int
    for range results {
        processedCount++
    }

    fmt.Printf("Processed %d jobs\n", processedCount)
}

func processJob(workerID, jobID int) string {
    // Simulate some processing work
    sum := 0
    for i := 0; i < 1000000; i++ {
        sum += i * jobID
    }

    return fmt.Sprintf("Worker %d processed job %d (sum: %d)", workerID, jobID, sum)
}