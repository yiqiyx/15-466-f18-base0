.PHONY : all

HOSTNAME := $(shell hostname)

ifeq ($(HOSTNAME), incepchow)
	BLENDER = ~/blender-2.79b-linux-glibc219-x86_64/blender
else
	BLENDER = blender
endif

DIST=../dist

all : \
	$(DIST)/stickochet.blob \


$(DIST)/stickochet.blob : stickochet.blend export-meshes.py
	$(BLENDER) --background --python export-meshes.py -- '$<' '$@'
