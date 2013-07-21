.PHONY: src/all src/clean src/depclean

src/all: src/varnishlog-buffer.exe

src/clean:
	$(RM) $(SRC_OBJECTS) $(CURDIR)/varnishlog-buffer.exe

src/depclean:
	$(RM) $(SRC_DEPS)

src/varnishlog-buffer.exe: $(ALL_OBJECTS)
src/varnishlog-buffer.exe: EXE_OBJECTS := $(ALL_OBJECTS)

-include $(SRC_DEPS)
