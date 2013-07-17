SRC_SOURCES := main.c die.c errors.c glib_extra.c priority.c varnishlog.c strings.c
SRC_SOURCES := $(SRC_SOURCES:%=$(CURDIR)/%)

SRC_OBJECTS := $(SRC_SOURCES:.c=.o)
SRC_OBJECTS := $(SRC_OBJECTS:.cpp=.o)

SRC_DEPS := $(SRC_OBJECTS:.o=.d)

ALL_OBJECTS := $(ALL_OBJECTS) $(SRC_OBJECTS)
