REBAR ?= $(shell which rebar3 2>/dev/null)

.PHONY: all compile test clean distclean

all: compile

compile:
	@${REBAR} compile

test: compile
	@${REBAR} eunit

clean:
	@${REBAR} clean

distclean: clean
	@rm -rf _build
	@rm -f priv/*.so priv/*.dylib
