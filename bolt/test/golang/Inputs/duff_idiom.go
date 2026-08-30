// DUFFZERO/DUFFCOPY ADR-idiom regression test for BOLT.
//
// The Go compiler on arm64 expands DUFFZERO/DUFFCOPY pseudo-instructions
// into a sequence that materializes the address of an intra-function
// return-address label with an ADR instruction (see
// cmd/internal/obj/arm64/obj7.go). BOLT used to treat such functions as
// non-simple because of the internal label reference and skipped (or, when
// an out-of-range ADR relaxation was needed, failed on) them.
//
// This program forces both idioms inside main.main: comparing non-pointer
// values boxed in interfaces inlines the runtime conversion code (which
// zeroes the freshly allocated object with DUFFZERO), and copying large
// structs through pointers emits DUFFCOPY.

package main

type Big struct {
	A, B, C, D, E, F, G, H     int64
	I, J, K, L, M, N, O, P     int64
}

func copyBig(dst, src *Big) {
	*dst = *src
}

func main() {
	var sum int64
	for i := int64(0); i < 100; i++ {
		// Interface conversions of non-pointer values inline convT
		// (DUFFZERO) into main.main on arm64.
		var a interface{} = i
		var b interface{} = i + 1
		if a == b {
			sum--
		} else {
			sum++
		}

		var x, y Big
		x.A = i
		y.P = i + 7
		copyBig(&x, &y)
		sum += x.P - x.A
	}
	println("sum=", sum)
	if sum != 5750 {
		panic("bad sum")
	}
}
