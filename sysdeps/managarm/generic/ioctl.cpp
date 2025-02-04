#include <errno.h>
#include <fcntl.h>
#include <linux/cdrom.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>

#include <bits/ensure.h>
#include <bragi/helpers-frigg.hpp>
#include <frg/vector.hpp>
#include <mlibc/all-sysdeps.hpp>
#include <mlibc/allocator.hpp>
#include <mlibc/debug.hpp>
#include <mlibc/posix-pipe.hpp>

#include <fs.frigg_bragi.hpp>
#include <posix.frigg_bragi.hpp>

namespace mlibc {

static constexpr bool logIoctls = false;

int ioctl_drm(int fd, unsigned long request, void *arg, int *result, HelHandle handle);

int sys_ioctl(int fd, unsigned long request, void *arg, int *result) {
	if(logIoctls)
		mlibc::infoLogger() << "mlibc: ioctl with"
				<< " type: 0x" << frg::hex_fmt(_IOC_TYPE(request))
				<< ", number: 0x" << frg::hex_fmt(_IOC_NR(request))
				<< " (raw request: " << frg::hex_fmt(request) << ")"
				<< " on fd " << fd << frg::endlog;

	SignalGuard sguard;
	auto handle = getHandleForFd(fd);
	if(!handle)
		return EBADF;

	if(_IOC_TYPE(request) == 'd') {
		return ioctl_drm(fd, request, arg, result, handle);
	}

	auto handle_siocgif = [&arg, &request, &result]
			(void (*req_setup)(managarm::fs::IfreqRequest<MemoryAllocator> &req, struct ifreq *ifr),
			int (*resp_parse)(managarm::fs::IfreqReply<MemoryAllocator> &resp, struct ifreq *ifr)) -> int {
		if(!arg)
			return EFAULT;

		auto ifr = reinterpret_cast<struct ifreq *>(arg);

		managarm::posix::NetserverRequest<MemoryAllocator> token_req(getSysdepsAllocator());
		managarm::fs::IfreqRequest<MemoryAllocator> req(getSysdepsAllocator());
		req.set_command(request);

		req_setup(req, ifr);

		auto [offer, send_token_req, send_req, recv_resp] =
		exchangeMsgsSync(
			getPosixLane(),
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(token_req, getSysdepsAllocator()),
				helix_ng::sendBragiHeadOnly(req, getSysdepsAllocator()),
				helix_ng::recvInline()
			)
		);

		HEL_CHECK(offer.error());
		HEL_CHECK(send_token_req.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::IfreqReply<MemoryAllocator> resp(getSysdepsAllocator());
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());

		int ret = resp_parse(resp, ifr);

		if(result)
			*result = 0;
		return ret;
	};

	managarm::fs::IoctlRequest<MemoryAllocator> ioctl_req(getSysdepsAllocator());

	switch(request) {
	case FIONBIO: {
		auto mode = reinterpret_cast<int *>(arg);
		int flags = fcntl(fd, F_GETFL, 0);
		if(*mode) {
		    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
		}else{
		    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
		}
		return 0;
	}
	case FIONREAD: {
		auto argp = reinterpret_cast<int *>(arg);

		auto handle = getHandleForFd(fd);
		if (!handle)
			return EBADF;

		if(!argp)
			return EINVAL;

		managarm::fs::GenericIoctlRequest<MemoryAllocator> req(getSysdepsAllocator());
		req.set_command(FIONREAD);

		auto [offer, send_ioctl_req, send_req, recv_resp] =
		exchangeMsgsSync(
			handle,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(ioctl_req, getSysdepsAllocator()),
				helix_ng::sendBragiHeadOnly(req, getSysdepsAllocator()),
				helix_ng::recvInline()
			)
		);

		HEL_CHECK(offer.error());
		HEL_CHECK(send_ioctl_req.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::GenericIoctlReply<MemoryAllocator> resp(getSysdepsAllocator());
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		if(resp.error() == managarm::fs::Errors::NOT_CONNECTED) {
			return ENOTCONN;
		} else {
			__ensure(resp.error() == managarm::fs::Errors::SUCCESS);

			*argp = resp.fionread_count();

			return 0;
		}
	}
	case FIOCLEX: {
		managarm::posix::IoctlFioclexRequest<MemoryAllocator> req(getSysdepsAllocator());
		req.set_fd(fd);

		auto [offer, sendReq, recvResp] = exchangeMsgsSync(
			getPosixLane(),
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, getSysdepsAllocator()),
				helix_ng::recvInline()
			)
		);

		HEL_CHECK(offer.error());
		HEL_CHECK(sendReq.error());
		if(recvResp.error() == kHelErrDismissed)
			return EINVAL;
		HEL_CHECK(recvResp.error());

		managarm::posix::SvrResponse<MemoryAllocator> resp(getSysdepsAllocator());
		resp.ParseFromArray(recvResp.data(), recvResp.length());
		__ensure(resp.error() == managarm::posix::Errors::SUCCESS);
		return 0;
	}
	case TCGETS: {
		auto param = reinterpret_cast<struct termios *>(arg);

		managarm::fs::GenericIoctlRequest<MemoryAllocator> req(getSysdepsAllocator());
		req.set_command(request);

		auto [offer, send_ioctl_req, send_req, recv_resp, recv_attrs] = exchangeMsgsSync(handle,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(ioctl_req, getSysdepsAllocator()),
				helix_ng::sendBragiHeadOnly(req, getSysdepsAllocator()),
				helix_ng::recvInline(),
				helix_ng::recvBuffer(param, sizeof(struct termios))
			)
		);

		HEL_CHECK(offer.error());
		HEL_CHECK(send_ioctl_req.error());
		if(send_req.error() == kHelErrDismissed)
			return EINVAL;
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());
		HEL_CHECK(recv_attrs.error());

		managarm::fs::GenericIoctlReply<MemoryAllocator> resp(getSysdepsAllocator());
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		__ensure(resp.error() == managarm::fs::Errors::SUCCESS);
		__ensure(recv_attrs.actualLength() == sizeof(struct termios));
		*result = resp.result();
		return 0;
	}
	case TCSETS: {
		auto param = reinterpret_cast<struct termios *>(arg);

		managarm::fs::GenericIoctlRequest<MemoryAllocator> req(getSysdepsAllocator());
		req.set_command(request);

		auto [offer, send_ioctl_req, send_req, send_attrs, recv_resp] = exchangeMsgsSync(handle,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(ioctl_req, getSysdepsAllocator()),
				helix_ng::sendBragiHeadOnly(req, getSysdepsAllocator()),
				helix_ng::sendBuffer(param, sizeof(struct termios)),
				helix_ng::recvInline()
			)
		);

		HEL_CHECK(offer.error());
		HEL_CHECK(send_ioctl_req.error());
		if(send_req.error() == kHelErrDismissed)
			return EINVAL;
		HEL_CHECK(send_req.error());
		HEL_CHECK(send_attrs.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::GenericIoctlReply<MemoryAllocator> resp(getSysdepsAllocator());
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		__ensure(resp.error() == managarm::fs::Errors::SUCCESS);
		if(result)
			*result = resp.result();
		return 0;
	}
	case TIOCSCTTY: {
		managarm::fs::GenericIoctlRequest<MemoryAllocator> req(getSysdepsAllocator());
		req.set_command(request);

		auto [offer, send_ioctl_req, send_req, imbue_creds, recv_resp] = exchangeMsgsSync(
			handle,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(ioctl_req, getSysdepsAllocator()),
				helix_ng::sendBragiHeadOnly(req, getSysdepsAllocator()),
				helix_ng::imbueCredentials(),
				helix_ng::recvInline())
		);

		HEL_CHECK(offer.error());
		if(send_req.error() == kHelErrDismissed)
			return EINVAL;
		HEL_CHECK(send_ioctl_req.error());
		HEL_CHECK(imbue_creds.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::GenericIoctlReply<MemoryAllocator> resp(getSysdepsAllocator());
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());

		if(resp.error() == managarm::fs::Errors::ILLEGAL_ARGUMENT) {
			return EINVAL;
		}else if(resp.error() == managarm::fs::Errors::INSUFFICIENT_PERMISSIONS) {
			return EPERM;
		}
		__ensure(resp.error() == managarm::fs::Errors::SUCCESS);
		*result = resp.result();
		return 0;
	}
	case TIOCGWINSZ: {
		auto param = reinterpret_cast<struct winsize *>(arg);

		managarm::fs::GenericIoctlRequest<MemoryAllocator> req(getSysdepsAllocator());
		req.set_command(request);

		auto [offer, send_ioctl_req, send_req, recv_resp] = exchangeMsgsSync(handle,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(ioctl_req, getSysdepsAllocator()),
				helix_ng::sendBragiHeadOnly(req, getSysdepsAllocator()),
				helix_ng::recvInline()
			)
		);

		HEL_CHECK(offer.error());
		HEL_CHECK(send_ioctl_req.error());
		if(send_req.error() == kHelErrDismissed)
			return EINVAL;
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::GenericIoctlReply<MemoryAllocator> resp(getSysdepsAllocator());
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		if(resp.error() == managarm::fs::Errors::ILLEGAL_OPERATION_TARGET)
			return EINVAL;
		__ensure(resp.error() == managarm::fs::Errors::SUCCESS);

		*result = resp.result();
		param->ws_col = resp.pts_width();
		param->ws_row = resp.pts_height();
		param->ws_xpixel = resp.pts_pixel_width();
		param->ws_ypixel = resp.pts_pixel_height();
		return 0;
	}
	case TIOCSWINSZ: {
		auto param = reinterpret_cast<const struct winsize *>(arg);

		managarm::fs::GenericIoctlRequest<MemoryAllocator> req(getSysdepsAllocator());
		req.set_command(request);
		req.set_pts_width(param->ws_col);
		req.set_pts_height(param->ws_row);
		req.set_pts_pixel_width(param->ws_xpixel);
		req.set_pts_pixel_height(param->ws_ypixel);

		auto [offer, send_ioctl_req, send_req, recv_resp] = exchangeMsgsSync(
			handle,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(ioctl_req, getSysdepsAllocator()),
				helix_ng::sendBragiHeadOnly(req, getSysdepsAllocator()),
				helix_ng::recvInline())
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_ioctl_req.error());
		if(send_req.error() == kHelErrDismissed)
			return EINVAL;
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::GenericIoctlReply<MemoryAllocator> resp(getSysdepsAllocator());
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		__ensure(resp.error() == managarm::fs::Errors::SUCCESS);

		*result = resp.result();
		return 0;
	}
	case TIOCGPTN: {
		auto param = reinterpret_cast<int *>(arg);

		managarm::fs::GenericIoctlRequest<MemoryAllocator> req(getSysdepsAllocator());
		req.set_command(request);

		auto [offer, send_ioctl_req, send_req, recv_resp] = exchangeMsgsSync(
			handle,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(ioctl_req, getSysdepsAllocator()),
				helix_ng::sendBragiHeadOnly(req, getSysdepsAllocator()),
				helix_ng::recvInline())
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_ioctl_req.error());
		if(send_req.error() == kHelErrDismissed)
			return EINVAL;
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::GenericIoctlReply<MemoryAllocator> resp(getSysdepsAllocator());
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		__ensure(resp.error() == managarm::fs::Errors::SUCCESS);
		*param = resp.pts_index();
		if(result)
			*result = resp.result();
		return 0;
	}
	case TIOCGPGRP: {
		managarm::fs::GenericIoctlRequest<MemoryAllocator> req(getSysdepsAllocator());
		req.set_command(request);

		frg::string<MemoryAllocator> ser(getSysdepsAllocator());
		req.SerializeToString(&ser);

		auto [offer, send_ioctl_req, send_req, imbue_creds, recv_resp] =
			exchangeMsgsSync(
					handle,
					helix_ng::offer(
							helix_ng::sendBragiHeadOnly(ioctl_req, getSysdepsAllocator()),
							helix_ng::sendBuffer(ser.data(), ser.size()),
							helix_ng::imbueCredentials(),
							helix_ng::recvInline()
					)
			);

		HEL_CHECK(offer.error());
		HEL_CHECK(send_ioctl_req.error());
		if(send_req.error())
			return EINVAL;
		HEL_CHECK(send_req.error());
		if(imbue_creds.error()) {
			infoLogger() << "mlibc: TIOCGPGRP used on unexpected socket, returning EINVAL (FIXME)" << frg::endlog;
			return EINVAL;
		}
		HEL_CHECK(imbue_creds.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::GenericIoctlReply<MemoryAllocator> resp(getSysdepsAllocator());
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		if(resp.error() == managarm::fs::Errors::NOT_A_TERMINAL) {
			return ENOTTY;
		}
		__ensure(resp.error() == managarm::fs::Errors::SUCCESS);
		*result = resp.result();
		*static_cast<int *>(arg) = resp.pid();
		return 0;
	}
	case TIOCSPGRP: {
		auto param = reinterpret_cast<int *>(arg);

		managarm::fs::GenericIoctlRequest<MemoryAllocator> req(getSysdepsAllocator());
		req.set_command(request);
		req.set_pgid(*param);

		frg::string<MemoryAllocator> ser(getSysdepsAllocator());
		req.SerializeToString(&ser);

		auto [offer, send_ioctl_req, send_req, imbue_creds, recv_resp] =
			exchangeMsgsSync(
					handle,
					helix_ng::offer(
							helix_ng::sendBragiHeadOnly(ioctl_req, getSysdepsAllocator()),
							helix_ng::sendBuffer(ser.data(), ser.size()),
							helix_ng::imbueCredentials(),
							helix_ng::recvInline()
					)
			);

		HEL_CHECK(offer.error());
		HEL_CHECK(send_ioctl_req.error());
		if(send_req.error() == kHelErrDismissed)
			return EINVAL;
		HEL_CHECK(send_req.error());
		HEL_CHECK(imbue_creds.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::GenericIoctlReply<MemoryAllocator> resp(getSysdepsAllocator());
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		if(resp.error() == managarm::fs::Errors::INSUFFICIENT_PERMISSIONS) {
			return EPERM;
		} else if(resp.error() == managarm::fs::Errors::ILLEGAL_ARGUMENT) {
			return EINVAL;
		}
		__ensure(resp.error() == managarm::fs::Errors::SUCCESS);
		*result = resp.result();
		return 0;
	}
	case TIOCGSID: {
		managarm::fs::GenericIoctlRequest<MemoryAllocator> req(getSysdepsAllocator());
		req.set_command(request);

		frg::string<MemoryAllocator> ser(getSysdepsAllocator());
		req.SerializeToString(&ser);

		auto [offer, send_ioctl_req, send_req, imbue_creds, recv_resp] =
			exchangeMsgsSync(
					handle,
					helix_ng::offer(
							helix_ng::sendBragiHeadOnly(ioctl_req, getSysdepsAllocator()),
							helix_ng::sendBuffer(ser.data(), ser.size()),
							helix_ng::imbueCredentials(),
							helix_ng::recvInline()
					)
			);

		HEL_CHECK(offer.error());
		if(send_ioctl_req.error())
			return EINVAL;
		HEL_CHECK(send_ioctl_req.error());
		if(send_req.error())
			return EINVAL;
		HEL_CHECK(send_req.error());
		if(imbue_creds.error() == kHelErrDismissed)
			return EINVAL;
		HEL_CHECK(imbue_creds.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::GenericIoctlReply<MemoryAllocator> resp(getSysdepsAllocator());
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		if(resp.error() == managarm::fs::Errors::NOT_A_TERMINAL) {
			return ENOTTY;
		}
		__ensure(resp.error() == managarm::fs::Errors::SUCCESS);
		*result = resp.result();
		*static_cast<int *>(arg) = resp.pid();
		return 0;
	}
	case CDROM_GET_CAPABILITY: {
		managarm::fs::GenericIoctlRequest<MemoryAllocator> req(getSysdepsAllocator());
		req.set_command(request);

		frg::string<MemoryAllocator> ser(getSysdepsAllocator());
		req.SerializeToString(&ser);

		auto [offer, send_ioctl_req, send_req, recv_resp] =
			exchangeMsgsSync(
					handle,
					helix_ng::offer(
						helix_ng::sendBragiHeadOnly(ioctl_req, getSysdepsAllocator()),
						helix_ng::sendBuffer(ser.data(), ser.size()),
						helix_ng::recvInline()
					)
			);

		HEL_CHECK(offer.error());
		if(send_ioctl_req.error())
			return EINVAL;
		HEL_CHECK(send_ioctl_req.error());
		if(send_req.error())
			return EINVAL;
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::GenericIoctlReply<MemoryAllocator> resp(getSysdepsAllocator());
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		if(resp.error() == managarm::fs::Errors::NOT_A_TERMINAL) {
			return ENOTTY;
		}
		__ensure(resp.error() == managarm::fs::Errors::SUCCESS);
		*result = resp.result();
		return 0;
	}
	} // end of switch()


	if(_IOC_TYPE(request) == 'E'
			&& _IOC_NR(request) == _IOC_NR(EVIOCGVERSION)) {
		*reinterpret_cast<int *>(arg) = 0x010001; // should be EV_VERSION
		*result = 0;
		return 0;
	}else if(_IOC_TYPE(request) == 'E'
			&& _IOC_NR(request) == _IOC_NR(EVIOCGID)) {
		memset(arg, 0, sizeof(struct input_id));
		*result = 0;
		return 0;
	}else if(_IOC_TYPE(request) == 'E'
			&& _IOC_NR(request) == _IOC_NR(EVIOCGNAME(0))) {
		const char *s = "Managarm generic evdev";
		auto chunk = frg::min(_IOC_SIZE(request), strlen(s) + 1);
		memcpy(arg, s, chunk);
		*result = chunk;
		return 0;
	}else if(_IOC_TYPE(request) == 'E'
			&& _IOC_NR(request) == _IOC_NR(EVIOCGPHYS(0))) {
		// Returns the sysfs path of the device.
		const char *s = "input0";
		auto chunk = frg::min(_IOC_SIZE(request), strlen(s) + 1);
		memcpy(arg, s, chunk);
		*result = chunk;
		return 0;
	}else if(_IOC_TYPE(request) == 'E'
			&& _IOC_NR(request) == _IOC_NR(EVIOCGUNIQ(0))) {
		// Returns a unique ID for the device.
		const char *s = "0";
		auto chunk = frg::min(_IOC_SIZE(request), strlen(s) + 1);
		memcpy(arg, s, chunk);
		*result = chunk;
		return 0;
	}else if(_IOC_TYPE(request) == 'E'
			&& _IOC_NR(request) == _IOC_NR(EVIOCGPROP(0))) {
		// Returns a bitmask of properties of the device.
		auto size = _IOC_SIZE(request);
		memset(arg, 0, size);
		*result = size;
		return 0;
	}else if(_IOC_TYPE(request) == 'E'
			&& _IOC_NR(request) == _IOC_NR(EVIOCGKEY(0))) {
		// Returns the current key state.
		auto size = _IOC_SIZE(request);
		memset(arg, 0, size);
		*result = size;
		return 0;
	}else if(_IOC_TYPE(request) == 'E'
			&& _IOC_NR(request) == _IOC_NR(EVIOCGLED(0))) {
		// Returns the current LED state.
		auto size = _IOC_SIZE(request);
		memset(arg, 0, size);
		*result = size;
		return 0;
	}else if(_IOC_TYPE(request) == 'E'
			&& _IOC_NR(request) == _IOC_NR(EVIOCGSW(0))) {
		auto size = _IOC_SIZE(request);
		memset(arg, 0, size);
		*result = size;
		return 0;
	}else if(_IOC_TYPE(request) == 'E'
			&& _IOC_NR(request) >= _IOC_NR(EVIOCGBIT(0, 0))
			&& _IOC_NR(request) <= _IOC_NR(EVIOCGBIT(EV_MAX, 0))) {
		// Returns a bitmask of capabilities of the device.
		// If type is zero, return a mask of supported types.
		// As EV_SYN is zero, this implies that it is impossible
		// to get the mask of supported synthetic events.
		auto type = _IOC_NR(request) - _IOC_NR(EVIOCGBIT(0, 0));
		if(!type) {
			// TODO: Check with the Linux ABI if we have to do this.
			memset(arg, 0, _IOC_SIZE(request));

			managarm::fs::GenericIoctlRequest<MemoryAllocator> req(getSysdepsAllocator());
			req.set_command(EVIOCGBIT(0, 0));
			req.set_size(_IOC_SIZE(request));

			auto [offer, send_ioctl_req, send_req, recv_resp, recv_data] = exchangeMsgsSync(
				handle,
				helix_ng::offer(
					helix_ng::sendBragiHeadOnly(ioctl_req, getSysdepsAllocator()),
					helix_ng::sendBragiHeadOnly(req, getSysdepsAllocator()),
					helix_ng::recvInline(),
					helix_ng::recvBuffer(arg, _IOC_SIZE(request)))
			);

			HEL_CHECK(offer.error());
			HEL_CHECK(send_ioctl_req.error());
			if(send_req.error() == kHelErrDismissed)
				return EINVAL;
			HEL_CHECK(send_req.error());
			HEL_CHECK(recv_resp.error());
			HEL_CHECK(recv_data.error());

			managarm::fs::GenericIoctlReply<MemoryAllocator> resp(getSysdepsAllocator());
			resp.ParseFromArray(recv_resp.data(), recv_resp.length());
			__ensure(resp.error() == managarm::fs::Errors::SUCCESS);
			*result = recv_data.actualLength();
			return 0;
		}else{
			// TODO: Check with the Linux ABI if we have to do this.
			memset(arg, 0, _IOC_SIZE(request));

			managarm::fs::GenericIoctlRequest<MemoryAllocator> req(getSysdepsAllocator());
			req.set_command(EVIOCGBIT(1, 0));
			req.set_input_type(type);
			req.set_size(_IOC_SIZE(request));

			auto [offer, send_ioctl_req, send_req, recv_resp, recv_data] = exchangeMsgsSync(
				handle,
				helix_ng::offer(
					helix_ng::sendBragiHeadOnly(ioctl_req, getSysdepsAllocator()),
					helix_ng::sendBragiHeadOnly(req, getSysdepsAllocator()),
					helix_ng::recvInline(),
					helix_ng::recvBuffer(arg, _IOC_SIZE(request)))
			);

			HEL_CHECK(offer.error());
			HEL_CHECK(send_ioctl_req.error());
			if(send_req.error() == kHelErrDismissed)
				return EINVAL;
			HEL_CHECK(send_req.error());
			HEL_CHECK(recv_resp.error());
			HEL_CHECK(recv_data.error());

			managarm::fs::GenericIoctlReply<MemoryAllocator> resp(getSysdepsAllocator());
			resp.ParseFromArray(recv_resp.data(), recv_resp.length());
			__ensure(resp.error() == managarm::fs::Errors::SUCCESS);
			*result = recv_data.actualLength();
			return 0;
		}
	}else if(_IOC_TYPE(request) == 'E'
			&& _IOC_NR(request) == _IOC_NR(EVIOCSCLOCKID)) {
		auto param = reinterpret_cast<int *>(arg);

		managarm::fs::GenericIoctlRequest<MemoryAllocator> req(getSysdepsAllocator());
		req.set_command(request);
		req.set_input_clock(*param);

		auto [offer, send_ioctl_req, send_req, recv_resp] = exchangeMsgsSync(handle,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(ioctl_req, getSysdepsAllocator()),
				helix_ng::sendBragiHeadOnly(req, getSysdepsAllocator()),
				helix_ng::recvInline()
			)
		);

		HEL_CHECK(offer.error());
		HEL_CHECK(send_ioctl_req.error());
		if(send_req.error() == kHelErrDismissed)
			return EINVAL;
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::GenericIoctlReply<MemoryAllocator> resp(getSysdepsAllocator());
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		__ensure(resp.error() == managarm::fs::Errors::SUCCESS);
		*result = resp.result();
		return 0;
	}else if(_IOC_TYPE(request) == 'E'
			&& _IOC_NR(request) >= _IOC_NR(EVIOCGABS(0))
			&& _IOC_NR(request) <= _IOC_NR(EVIOCGABS(ABS_MAX))) {
		auto param = reinterpret_cast<struct input_absinfo *>(arg);

		auto type = _IOC_NR(request) - _IOC_NR(EVIOCGABS(0));
		managarm::fs::GenericIoctlRequest<MemoryAllocator> req(getSysdepsAllocator());
		req.set_command(EVIOCGABS(0));
		req.set_input_type(type);

		auto [offer, send_ioctl_req, send_req, recv_resp] = exchangeMsgsSync(handle,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(ioctl_req, getSysdepsAllocator()),
				helix_ng::sendBragiHeadOnly(req, getSysdepsAllocator()),
				helix_ng::recvInline()
			)
		);

		HEL_CHECK(offer.error());
		HEL_CHECK(send_ioctl_req.error());
		if(send_req.error() == kHelErrDismissed)
			return EINVAL;
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::fs::GenericIoctlReply<MemoryAllocator> resp(getSysdepsAllocator());
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		__ensure(resp.error() == managarm::fs::Errors::SUCCESS);

		param->value = resp.input_value();
		param->minimum = resp.input_min();
		param->maximum = resp.input_max();
		param->fuzz = resp.input_fuzz();
		param->flat = resp.input_flat();
		param->resolution = resp.input_resolution();

		*result = resp.result();
		return 0;
	}else if(request == KDSETMODE) {
		auto param = reinterpret_cast<unsigned int *>(arg);
		mlibc::infoLogger() << "\e[35mmlibc: KD_SETMODE(" << frg::hex_fmt(param) << ") is a no-op" << frg::endlog;

		*result = 0;
		return 0;
	}else if(request == KDGETMODE) {
		auto param = reinterpret_cast<unsigned int *>(arg);
		mlibc::infoLogger() << "\e[35mmlibc: KD_GETMODE is a no-op" << frg::endlog;
		*param = 0;

		*result = 0;
		return 0;
	}else if(request == KDSKBMODE) {
		auto param = reinterpret_cast<long>(arg);
		mlibc::infoLogger() << "\e[35mmlibc: KD_SKBMODE(" << frg::hex_fmt(param) << ") is a no-op" << frg::endlog;

		*result = 0;
		return 0;
	}else if(request == VT_SETMODE) {
		// auto param = reinterpret_cast<struct vt_mode *>(arg);
		mlibc::infoLogger() << "\e[35mmlibc: VT_SETMODE is a no-op" << frg::endlog;

		*result = 0;
		return 0;
	}else if(request == VT_GETSTATE) {
		auto param = reinterpret_cast<struct vt_stat *>(arg);

		param->v_active = 0;
		param->v_signal = 0;
		param->v_state = 0;

		mlibc::infoLogger() << "\e[35mmlibc: VT_GETSTATE is a no-op" << frg::endlog;

		*result = 0;
		return 0;
	}else if(request == VT_ACTIVATE || request == VT_WAITACTIVE) {
		mlibc::infoLogger() << "\e[35mmlibc: VT_ACTIVATE/VT_WAITACTIVE are no-ops" << frg::endlog;
		*result = 0;
		return 0;
	}else if(request == TIOCSPTLCK) {
		mlibc::infoLogger() << "\e[35mmlibc: TIOCSPTLCK is a no-op" << frg::endlog;
		if(result)
			*result = 0;
		return 0;
	} else if(request == SIOCGIFNAME) {
		return handle_siocgif([](auto req, auto ifr) {
			req.set_index(ifr->ifr_ifindex);
		}, [](auto resp, auto ifr) {
			if(resp.error() != managarm::fs::Errors::SUCCESS)
				return EINVAL;
			strncpy(ifr->ifr_name, resp.name().data(), IFNAMSIZ);
			return 0;
		});
	}else if(request == SIOCGIFCONF) {
		if(!arg)
			return EFAULT;

		auto ifc = reinterpret_cast<struct ifconf *>(arg);

		managarm::posix::NetserverRequest<MemoryAllocator> token_req(getSysdepsAllocator());
		managarm::fs::IfreqRequest<MemoryAllocator> req(getSysdepsAllocator());
		req.set_command(request);

		auto [offer, send_token_req, send_req, recv_resp] =
		exchangeMsgsSync(
			getPosixLane(),
			helix_ng::offer(
				helix_ng::want_lane,
				helix_ng::sendBragiHeadOnly(token_req, getSysdepsAllocator()),
				helix_ng::sendBragiHeadOnly(req, getSysdepsAllocator()),
				helix_ng::recvInline()
			)
		);

		auto conversation = offer.descriptor();

		HEL_CHECK(offer.error());
		HEL_CHECK(send_token_req.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		auto preamble = bragi::read_preamble(recv_resp);
		__ensure(!preamble.error());

		frg::vector<uint8_t, MemoryAllocator> tailBuffer{getSysdepsAllocator()};
		tailBuffer.resize(preamble.tail_size());
		auto [recv_tail] = exchangeMsgsSync(
			conversation.getHandle(),
			helix_ng::recvBuffer(tailBuffer.data(), tailBuffer.size())
		);

		HEL_CHECK(recv_tail.error());

		auto resp = *bragi::parse_head_tail<managarm::fs::IfconfReply>(recv_resp, tailBuffer, getSysdepsAllocator());
		recv_resp.reset();

		__ensure(resp.error() == managarm::fs::Errors::SUCCESS);

		if(ifc->ifc_buf == nullptr) {
			ifc->ifc_len = int(resp.ifconf_size() * sizeof(struct ifreq));
			return 0;
		}

		ifc->ifc_len = frg::min(int(resp.ifconf_size() * sizeof(struct ifreq)), ifc->ifc_len);

		for(size_t i = 0; i < frg::min(resp.ifconf_size() , ifc->ifc_len / sizeof(struct ifreq)); ++i) {
			auto &conf = resp.ifconf()[i];

			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_addr.s_addr = htonl(conf.ip4());

			ifreq *req = &ifc->ifc_req[i];
			strncpy(req->ifr_name, conf.name().data(), IFNAMSIZ);
			memcpy(&req->ifr_addr, &addr, sizeof(addr));
		}

		if(result)
			*result = 0;
		return 0;
	}else if(request == SIOCGIFNETMASK) {
		return handle_siocgif([](auto req, auto ifr) {
			req.set_name(frg::string<MemoryAllocator>{ifr->ifr_name, getSysdepsAllocator()});
		}, [](auto resp, auto ifr) {
			if(resp.error() != managarm::fs::Errors::SUCCESS)
				return EINVAL;

			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_addr = { htonl(resp.ip4_netmask()) };
			memcpy(&ifr->ifr_netmask, &addr, sizeof(addr));

			return 0;
		});
	}else if(request == SIOCGIFINDEX) {
		return handle_siocgif([](auto req, auto ifr) {
			req.set_name(frg::string<MemoryAllocator>{ifr->ifr_name, getSysdepsAllocator()});
		}, [](auto resp, auto ifr) {
			if(resp.error() != managarm::fs::Errors::SUCCESS)
				return EINVAL;
			ifr->ifr_ifindex = resp.index();
			return 0;
		});
	}else if(request == SIOCGIFFLAGS) {
		return handle_siocgif([](auto req, auto ifr) {
			req.set_name(frg::string<MemoryAllocator>{ifr->ifr_name, getSysdepsAllocator()});
		}, [](auto resp, auto ifr) {
			if(resp.error() != managarm::fs::Errors::SUCCESS)
				return EINVAL;
			ifr->ifr_flags = resp.flags();
			return 0;
		});
	}else if(request == SIOCGIFADDR) {
		return handle_siocgif([](auto req, auto ifr) {
			req.set_name(frg::string<MemoryAllocator>{ifr->ifr_name, getSysdepsAllocator()});
		}, [](auto resp, auto ifr) {
			if(resp.error() != managarm::fs::Errors::SUCCESS)
				return EINVAL;

			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_addr = { htonl(resp.ip4_addr()) };
			memcpy(&ifr->ifr_addr, &addr, sizeof(addr));

			return 0;
		});
	}else if(request == SIOCGIFMTU) {
		return handle_siocgif([](auto req, auto ifr) {
			req.set_name(frg::string<MemoryAllocator>{ifr->ifr_name, getSysdepsAllocator()});
		}, [](auto resp, auto ifr) {
			if(resp.error() != managarm::fs::Errors::SUCCESS)
				return EINVAL;

			ifr->ifr_mtu = resp.mtu();

			return 0;
		});
	}

	mlibc::infoLogger() << "mlibc: Unexpected ioctl with"
			<< " type: 0x" << frg::hex_fmt(_IOC_TYPE(request))
			<< ", number: 0x" << frg::hex_fmt(_IOC_NR(request))
			<< " (raw request: " << frg::hex_fmt(request) << ")" << frg::endlog;
	__ensure(!"Illegal ioctl request");
	__builtin_unreachable();
}

} //namespace mlibc
