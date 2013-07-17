.PHONY: src/all src/clean src/depclean

src/all: src/academia-varnishlog.exe

src/clean:
	$(RM) $(SRC_OBJECTS) $(CURDIR)/academia-varnishlog.exe

src/depclean:
	$(RM) $(SRC_DEPS)

src/academia-varnishlog.exe: $(ALL_OBJECTS)
src/academia-varnishlog.exe: EXE_OBJECTS := $(ALL_OBJECTS)

-include $(SRC_DEPS)
