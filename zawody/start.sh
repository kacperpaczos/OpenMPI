#!/bin/bash

# Usuwanie wcześniejszego pliku hello
rm -f a.out

# Kompilacja programu
make

# Uruchomienie programu
mpiexec --oversubscribe -n 2 ./a.out 1
