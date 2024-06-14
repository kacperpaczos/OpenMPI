#!/bin/bash

# Usuwanie wcześniejszego pliku hello
rm -f a.out

# Kompilacja programu
make

# Uruchomienie programu
mpiexec --oversubscribe -n 20 ./a.out 4
