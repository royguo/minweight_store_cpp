//go:build ignore

package main

import (
	"bufio"
	"fmt"
	"math/rand"
	"os"
)

func main() {
	for _, n := range []int{1_000, 10_000, 100_000} {
		if err := writeKeys(n); err != nil {
			panic(err)
		}
	}
}

func writeKeys(n int) error {
	path := fmt.Sprintf("bench_keys_%dK.tsv", n/1000)
	file, err := os.Create(path)
	if err != nil {
		return err
	}
	defer file.Close()

	out := bufio.NewWriter(file)
	defer out.Flush()

	rng := rand.New(rand.NewSource(int64(n)))
	seen := make(map[string]struct{}, n)
	for len(seen) < n {
		key := fmt.Sprintf("key-%08x-%08x", rng.Uint32(), rng.Uint32())
		if _, ok := seen[key]; ok {
			continue
		}
		seen[key] = struct{}{}
		if _, err := fmt.Fprintln(out, key); err != nil {
			return err
		}
	}
	return nil
}
