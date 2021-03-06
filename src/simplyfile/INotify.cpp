#include "INotify.h"

#include <linux/limits.h>

#include <iostream>

namespace simplyfile {

INotify::INotify(int flags)
	: FileDescriptor(::inotify_init1(flags))
{}

void INotify::watch(std::string const& _path, uint32_t mask) {
	int id = inotify_add_watch(*this, _path.c_str(), mask);
	mIDs[id] = _path;
}

auto INotify::readEvent() -> std::optional<INotify::Result> {
	auto maxSize = sizeof(inotify_event) + NAME_MAX + 1;
	auto buffer = read(static_cast<FileDescriptor&>(*this), maxSize, true);
	inotify_event const& event = *reinterpret_cast<inotify_event const*>(buffer.data());

	if (0 == event.wd) {
		return std::nullopt;
	}

	INotify::Result res;
	res.path = mIDs.at(event.wd);
	if (event.len > 0) {
		res.file = event.name;
	}

	return res;
}

auto read(INotify fd) -> std::optional<INotify::Result> {
	return fd.readEvent();
}

}

