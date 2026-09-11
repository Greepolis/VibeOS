cd /mnt/c/Users/Stefa/Documents/Progetti/VibeOS
cmake --build build-clang-Release -j8 2>&1 | grep -E 'error:' | head -3
for i in 1 2 3; do
  python3 scripts/qemu-cli-smoke-linux.py build-clang-Release 300 >/dev/null 2>&1
  echo "  boot$i $(grep -o 'reason=[^ ]*' qemu-cli-summary.txt | head -1)"
done
