#!/bin/bash

# Usuwanie wcześniejszego pliku hello
rm -f a.out

# Kompilacja programu
make

# Uruchomienie programu
mpiexec -n 3 ./a.out 1
