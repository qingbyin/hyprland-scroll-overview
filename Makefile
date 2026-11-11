CXX ?= g++

EXTRA_FLAGS =
LUA_PKG ?= $(shell pkg-config --exists 'lua5.4 >= 5.4' && echo lua5.4 || echo lua)
VERSION_HEADER = .build/PluginVersion.hpp
VERSION_SCRIPT = scripts/generate-plugin-version.sh

ifeq ($(CXX),g++)
    EXTRA_FLAGS += -fno-gnu-unique
endif

.PHONY: all clean test test-workspace-titles test-window-search test-input-hooks FORCE

all: $(VERSION_HEADER)
	$(CXX) -shared -fPIC $(EXTRA_FLAGS) -I.build main.cpp Config.cpp DropIndicator.cpp NativeDrag.cpp OverviewGesture.cpp OverviewManager.cpp OverviewPassElement.cpp OverviewRender.cpp OverviewSearch.cpp SearchInput.cpp WindowSearch.cpp Window.cpp WorkspaceTitles.cpp WorkspaceTitleUI.cpp scrollOverview.cpp -o scrolloverview.so -g `pkg-config --cflags pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon '$(LUA_PKG) >= 5.4'` -std=c++2b -Wno-narrowing

$(VERSION_HEADER): FORCE $(VERSION_SCRIPT)
	sh $(VERSION_SCRIPT) $@ .

FORCE:

clean:
	rm -f ./scrolloverview.so $(VERSION_HEADER)

test: test-workspace-titles test-window-search test-input-hooks

test-workspace-titles:
	bash scripts/test-workspace-titles.sh

test-window-search:
	./scripts/test-window-search.sh

test-input-hooks:
	./scripts/test-input-hooks.sh
