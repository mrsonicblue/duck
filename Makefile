.PHONY: all release

all:
	$(MAKE) -C fs

clean:
	$(MAKE) -C fs clean
	$(Q)rm -rf release

release:
	$(Q)mkdir -p release/duck
	$(Q)cp fs/duckfs release/duck/
	$(Q)cd release && $(Q)tar cvfz duck.tgz duck