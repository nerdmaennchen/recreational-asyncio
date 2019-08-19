#include <iostream>
#include "asyncio/AsyncIO.h"

#include "simplyfile/Timer.h"
#include "simplyfile/socket/Socket.h"

#include <future>
#include <chrono>

int main() {
    asyncio::Scheduler scheduler;
    using namespace std::chrono_literals;
    
    simplyfile::ServerSocket ss{simplyfile::makeUnixDomainHost("mysock")};
    ss.listen();
    auto server = [](simplyfile::ServerSocket ss) -> void {
        while (true) {
            asyncio::await(asyncio::readable(ss));
            simplyfile::ClientSocket cs = ss.accept();
            cs.setFlags(O_NONBLOCK);
            std::cout << "client connected" << std::endl;
            std::vector<std::byte> buf{4096};
            while (true) {
                try {
                    asyncio::await(asyncio::readable(cs));
                    auto r = read(cs, buf.data(), buf.size());
                    if (r == 0) {
                        break;
                    }
                    while (r) {
                        asyncio::await(asyncio::writable(cs));
                        auto written = write(cs, buf.data(), r);
                        if (written == -1) {
                            throw std::runtime_error("client gone away");
                        }
                        r -= written;
                    }
                } catch (std::exception const& e) {
                    std::cout << e.what() << std::endl;
                    break;
                }
            }
            std::cout << "client disconnected" << std::endl;
        }
    };

    auto client = [&]() -> void {
        while (true) {
            {
                simplyfile::ClientSocket cs{simplyfile::makeUnixDomainHost("mysock")};
                cs.setFlags(O_NONBLOCK);
                cs.connect();
                asyncio::await(asyncio::writable(cs));
                std::string buf = "hallo Welt";
                write(cs, buf.data(), buf.size());

                asyncio::await(asyncio::readable(cs));
                auto r = read(cs, buf.data(), buf.size());
                buf.resize(r);
                std::cout << buf << std::endl;
            }
            asyncio::await(asyncio::readable(simplyfile::Timer{1s}));
        }
    };

    auto servTask = scheduler.run(server, std::move(ss));
    auto clientTask = scheduler.run(client);

    auto tasks_done = [](auto const&... t) {
        return (... and (t.wait_until(std::chrono::system_clock::time_point{}) == std::future_status::ready));
    };

    while (not tasks_done(servTask, clientTask)) {
        scheduler.work();
    }
	std::cout << "done" << std::endl;
	return 0;
}