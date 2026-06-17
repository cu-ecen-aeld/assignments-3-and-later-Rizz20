#!/bin/sh

if [ $# -ne 2 ]; then
    echo "Error: Two arguments required: <writefile> <writestr>"
    exit 1
fi

writefile=$1
writestr=$2

# Create the directory path if it doesn't exist
dir=$(dirname "$writefile")
if [ ! -d "$dir" ]; then
    mkdir -p "$dir"
    if [ $? -ne 0 ]; then
        echo "Error: Could not create directory $dir"
        exit 1
    fi
fi

# Write the string to the file, overwriting if it exists
echo "$writestr" > "$writefile"

if [ $? -ne 0 ]; then
    echo "Error: Could not create or write to file $writefile"
    exit 1
fi
