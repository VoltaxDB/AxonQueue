#!/bin/bash

CLANG_FORMAT=clang-format

echo "📁 Formatting all .cpp, .h, and .hpp files in the current directory..."

find . -type f \( -iname "*.cpp" -o -iname "*.h" -o -iname "*.hpp" \) | while read -r file; do
    echo "✨ Formatting $file"
    $CLANG_FORMAT -i "$file"
done

echo "✅ All files formatted successfully!"
