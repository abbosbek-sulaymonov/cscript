package main

import "fmt"

func main() {
	sum := 0
	for i := 0; i < 10000000; i++ {
		sum += i % 7
	}
	fmt.Println(sum)
}
