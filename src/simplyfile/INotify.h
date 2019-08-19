#include "FileDescriptor.h"

#include <map>
#include <optional>
#include <string>

#include <sys/inotify.h>

namespace simplyfile {

struct INotify : FileDescriptor {
	using FileDescriptor::FileDescriptor;
	INotify(int flags=0);

	// check inotify for mask meaning
	void watch(std::string const& _path, uint32_t mask);

	std::map<int, std::string> mIDs;

	struct Result {
		std::string path;
		std::string file;
	};

	[[nodiscard]] auto readEvent() -> std::optional<INotify::Result>;
};

[[nodiscard]] auto read(INotify fd) -> std::optional<INotify::Result>;

}
