TARGET   := exfs2

all:
	gcc main.c -o $(TARGET)

reset:
	rm -f dataseg{0..500} inodeseg{0..500}

clean:
	rm -f $(TARGET) dataseg{0..500} inodeseg{0..500}

check:
	#
	#
	# 1. Adding a file to /dir1/dir2/dir3/sample.txt from ./sample.txt 
	@./$(TARGET) -a /dir1/dir2/dir3/sample.txt -f ./sample.txt && echo " OK: added file /dir1/dir2/dir3/sample.txt from ./sample.txt"

	#
	#
	# 2. Listing and checking the file in /dir1/dir2/dir3/sample.txt exists
	@./$(TARGET) -l | grep -q "dir1" && echo " OK: saw 'dir1' directory"
	@./$(TARGET) -l | grep -q "dir2" && echo " OK: saw 'dir2' directory"
	@./$(TARGET) -l | grep -q "dir3" && echo " OK: saw 'dir3' directory"
	@./$(TARGET) -l | grep -q "sample.txt" && echo " OK: saw 'sample.txt' file"

	#
	#
	# 3. Extracting content of the file /dir1/dir2/dir3/sample.txt to output.txt
	@./$(TARGET) -e dir1/dir2/dir3/sample.txt > output.txt && echo " OK: extracted content of /dir1/dir2/dir3/sample.txt to output.txt"

	#
	#
	# 4. Comparing sample.txt with output.txt whether they are the same
	@diff -q sample.txt output.txt && echo " OK: sample.txt and output.txt are the same" || echo " ERROR: sample.txt and output.txt are different"

	#
	#
	# 5. Removing the file from /dir1/dir2/dir3/sample.txt
	@./$(TARGET) -r /dir1/dir2/dir3/sample.txt
	@echo " OK: removed file /dir1/dir2/dir3/sample.txt"

	#
	#
	# 6. Listing and checking the file /dir1/dir2/dir3/sample.txt does not exist
	@./$(TARGET) -l | grep -q "dir1" && echo " OK: saw 'dir1' directory"
	@./$(TARGET) -l | grep -q "dir2" && echo " OK: saw 'dir2' directory"
	@./$(TARGET) -l | grep -q "dir3" && echo " OK: saw 'dir3' directory"
	@./$(TARGET) -l | grep -q "sample.txt" && echo " ERROR: saw 'sample.txt' file" || echo " OK: did not see 'sample.txt' file"

	#
	#
	# 7. Removing the directory /dir1/dir2/dir3
	@./$(TARGET) -r /dir1/dir2/dir3
	@echo " OK: removed directory /dir1/dir2/dir3"

	#
	#
	# 8. Listing and checking the directory /dir1/dir2/dir3 does not exist
	@./$(TARGET) -l | grep -q "dir1" && echo " OK: saw 'dir1' directory"
	@./$(TARGET) -l | grep -q "dir2" && echo " OK: saw 'dir2' directory"
	@./$(TARGET) -l | grep -q "dir3" && echo " ERROR: saw 'dir3' directory" || echo " OK: did not see 'dir3' directory"

	#
	#
	@echo "✅ All tests passed!"

	