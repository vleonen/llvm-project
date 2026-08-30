// run

package main

import (
	"runtime"
	"runtime/debug"
	"sync/atomic"
)

type stackTest struct {
	counter int32
}

func (s *stackTest) stackFunc() int {
	a := []int{1, 2, 3, 4, 5}
	sum := 0
	for _, v := range a {
		sum += v
	}
	local := int32(sum * 100)
	atomic.AddInt32(&s.counter, local)
	return int(local)
}

//go:noinline
func worker(fn func(*stackTest) int, s *stackTest, iters int) {
	for i := 0; i < iters; i++ {
		_ = make([]byte, 1024)
		runtime.Gosched()
		fn(s)
		runtime.Gosched()
	}
}

func deepWorker(fn func(*stackTest) int, s *stackTest, iters, depth int) {
	_ = make([]byte, 1024)
	if depth > 0 {
		runtime.Gosched()
		deepWorker(fn, s, iters, depth-1)
		runtime.Gosched()
	} else {
		worker(fn, s, iters)
	}
}

func main() {
	debug.SetGCPercent(1)
	fn := (*stackTest).stackFunc
	var s stackTest

	for i := 0; i < 8; i++ {
		go deepWorker(fn, &s, 20, 40)
	}

	atomic.LoadInt32(&s.counter)
}
