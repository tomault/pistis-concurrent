#include "EpollEventType.hpp"
#include <tuple>
#include <vector>

using namespace pistis::concurrent;

namespace {
  
  static const std::vector< std::tuple<EpollEventType, std::string> >&
      eventToNameMap() {
    static const std::vector< std::tuple<EpollEventType, std::string> >
        EVENT_TO_NAME_MAP{
      std::tuple<EpollEventType, std::string>(EpollEventType::READ, "READ"),
      std::tuple<EpollEventType, std::string>(EpollEventType::WRITE, "WRITE"),
      std::tuple<EpollEventType, std::string>(EpollEventType::READ_HANGUP,
					      "READ_HANGUP"),
      std::tuple<EpollEventType, std::string>(EpollEventType::HANGUP,
					      "HANGUP"),
      std::tuple<EpollEventType, std::string>(EpollEventType::PRIORITY,
					      "PRIORITY"),
      std::tuple<EpollEventType, std::string>(EpollEventType::ERROR, "ERROR")
    };
    return EVENT_TO_NAME_MAP;
  }
  
}

namespace pistis {
  namespace concurrent {

    std::ostream& operator<<(std::ostream& out, EpollEventType events) {
      if (events == EpollEventType::NONE) {
	return out << "NONE";
      } else {
	int cnt = 0;
	for (const auto& event : eventToNameMap()) {
	  if ((events & std::get<0>(event)) != EpollEventType::NONE) {
	    if (cnt) {
	      out << "|";
	    }
	    out << std::get<1>(event);
	    ++cnt;
	  }
	}
	return out;
      }
    }
    
  }
}
