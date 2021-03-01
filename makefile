include config.mk
# shell中的for循环, shell里边的变量用两个$
all:  
	@for dir in $(BUILD_DIR); \
	do \
		make -C $$dir; \
	done

clean:
	rm -rf app/link_obj app/dep nginx
