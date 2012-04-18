/*
	Copyright (C) 2006,2007 John Anderson
	Copyright (C) 2012 Paul Davis

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include <sstream>
#include <stdint.h>
#include "strip.h"

#include <sys/time.h>

#include "midi++/port.h"

#include "pbd/compose.h"
#include "pbd/convert.h"

#include "ardour/amp.h"
#include "ardour/bundle.h"
#include "ardour/debug.h"
#include "ardour/midi_ui.h"
#include "ardour/meter.h"
#include "ardour/pannable.h"
#include "ardour/panner.h"
#include "ardour/panner_shell.h"
#include "ardour/rc_configuration.h"
#include "ardour/route.h"
#include "ardour/session.h"
#include "ardour/send.h"
#include "ardour/track.h"
#include "ardour/user_bundle.h"

#include "mackie_control_protocol.h"
#include "surface_port.h"
#include "surface.h"
#include "button.h"
#include "led.h"
#include "pot.h"
#include "fader.h"
#include "jog.h"
#include "meter.h"

using namespace Mackie;
using namespace std;
using namespace ARDOUR;
using namespace PBD;

#define ui_context() MackieControlProtocol::instance() /* a UICallback-derived object that specifies the event loop for signal handling */
#define ui_bind(f, ...) boost::protect (boost::bind (f, __VA_ARGS__))

extern PBD::EventLoop::InvalidationRecord* __invalidator (sigc::trackable& trackable, const char*, int);
#define invalidator() __invalidator (*(MackieControlProtocol::instance()), __FILE__, __LINE__)

Strip::Strip (Surface& s, const std::string& name, int index, const map<Button::ID,StripButtonInfo>& strip_buttons)
	: Group (name)
	, _solo (0)
	, _recenable (0)
	, _mute (0)
	, _select (0)
	, _vselect (0)
	, _fader_touch (0)
	, _vpot (0)
	, _vpot_mode (PanAzimuth)
	, _preflip_vpot_mode (PanAzimuth)
	, _fader (0)
	, _index (index)
	, _surface (&s)
	, _controls_locked (false)
	, _reset_display_at (0)
	, _last_gain_position_written (-1.0)
	, _last_pan_position_written (-1.0)
{
	_fader = dynamic_cast<Fader*> (Fader::factory (*_surface, index, "fader", *this));
	_vpot = dynamic_cast<Pot*> (Pot::factory (*_surface, Pot::ID + index, "vpot", *this));
	_meter = dynamic_cast<Meter*> (Meter::factory (*_surface, index, "meter", *this));

	for (map<Button::ID,StripButtonInfo>::const_iterator b = strip_buttons.begin(); b != strip_buttons.end(); ++b) {
		Button* bb = dynamic_cast<Button*> (Button::factory (*_surface, b->first, b->second.base_id + index, b->second.name, *this));
		DEBUG_TRACE (DEBUG::MackieControl, string_compose ("surface %1 strip %2 new button BID %3 id %4 from base %5\n",
								   _surface->number(), index, Button::id_to_name (bb->bid()), 
								   bb->id(), b->second.base_id));
	}
}	

Strip::~Strip ()
{
	/* surface is responsible for deleting all controls */
}

void 
Strip::add (Control & control)
{
	Button* button;

	Group::add (control);

	/* fader, vpot, meter were all set explicitly */

	if ((button = dynamic_cast<Button*>(&control)) != 0) {
		switch (button->bid()) {
		case Button::RecEnable:
			_recenable = button;
			break;
		case Button::Mute:
			_mute = button;
			break;
		case Button::Solo:
			_solo = button;
			break;
		case Button::Select:
			_select = button;
			break;
		case Button::VSelect:
			_vselect = button;
			break;
		case Button::FaderTouch:
			_fader_touch = button;
		default:
			break;
		}
	}
}

void
Strip::set_route (boost::shared_ptr<Route> r)
{
	if (_controls_locked) {
		return;
	}

	route_connections.drop_connections ();
	
	_solo->set_control (boost::shared_ptr<AutomationControl>());
	_mute->set_control (boost::shared_ptr<AutomationControl>());
	_select->set_control (boost::shared_ptr<AutomationControl>());
	_recenable->set_control (boost::shared_ptr<AutomationControl>());
	_fader->set_control (boost::shared_ptr<AutomationControl>());
	_vpot->set_control (boost::shared_ptr<AutomationControl>());

	_route = r;

	if (!r) {
		return;
	}

	DEBUG_TRACE (DEBUG::MackieControl, string_compose ("Surface %1 strip %2 now mapping route %3\n",
							   _surface->number(), _index, _route->name()));
	
	_solo->set_control (_route->solo_control());
	_mute->set_control (_route->mute_control());
	set_vpot_mode (PanAzimuth);
	
	_route->solo_control()->Changed.connect(route_connections, invalidator(), ui_bind (&Strip::notify_solo_changed, this), ui_context());
	_route->mute_control()->Changed.connect(route_connections, invalidator(), ui_bind (&Strip::notify_mute_changed, this), ui_context());

	boost::shared_ptr<Pannable> pannable = _route->pannable();

	if (pannable) {
		pannable->pan_azimuth_control->Changed.connect(route_connections, invalidator(), ui_bind (&Strip::notify_panner_changed, this, false), ui_context());
		pannable->pan_width_control->Changed.connect(route_connections, invalidator(), ui_bind (&Strip::notify_panner_changed, this, false), ui_context());
	}
	_route->gain_control()->Changed.connect(route_connections, invalidator(), ui_bind (&Strip::notify_gain_changed, this, false), ui_context());
	_route->PropertyChanged.connect (route_connections, invalidator(), ui_bind (&Strip::notify_property_changed, this, _1), ui_context());
	
	boost::shared_ptr<Track> trk = boost::dynamic_pointer_cast<ARDOUR::Track>(_route);
	
	if (trk) {
		_recenable->set_control (trk->rec_enable_control());
		trk->rec_enable_control()->Changed .connect(route_connections, invalidator(), ui_bind (&Strip::notify_record_enable_changed, this), ui_context());
	}
	
	// TODO this works when a currently-banked route is made inactive, but not
	// when a route is activated which should be currently banked.
	
	_route->active_changed.connect (route_connections, invalidator(), ui_bind (&Strip::notify_active_changed, this), ui_context());
	_route->DropReferences.connect (route_connections, invalidator(), ui_bind (&Strip::notify_route_deleted, this), ui_context());
	
	/* Update */
	
	notify_all ();

	/* setup legal VPot modes for this route */
	
	build_input_list (_route->input()->n_ports());
	build_output_list (_route->output()->n_ports());

	current_pot_modes.clear();

	if (pannable) {
		boost::shared_ptr<Panner> panner = pannable->panner();
		if (panner) {
			set<Evoral::Parameter> automatable = panner->what_can_be_automated ();
			set<Evoral::Parameter>::iterator a;
			
			if ((a = automatable.find (PanAzimuthAutomation)) != automatable.end()) {
				current_pot_modes.push_back (PanAzimuth);
			}
			
			if ((a = automatable.find (PanWidthAutomation)) != automatable.end()) {
				current_pot_modes.push_back (PanWidth);
			}
		}
	}

	current_pot_modes.push_back (Input);
	current_pot_modes.push_back (Output);

	if (_route->nth_send (0) != 0) {
		current_pot_modes.push_back (Send1);
	}
	if (_route->nth_send (1) != 0) {
		current_pot_modes.push_back (Send2);
	}
	if (_route->nth_send (2) != 0) {
		current_pot_modes.push_back (Send3);
	}
	if (_route->nth_send (3) != 0) {
		current_pot_modes.push_back (Send4);
	}
	if (_route->nth_send (4) != 0) {
		current_pot_modes.push_back (Send5);
	}
	if (_route->nth_send (5) != 0) {
		current_pot_modes.push_back (Send6);
	}
	if (_route->nth_send (6) != 0) {
		current_pot_modes.push_back (Send7);
	}
	if (_route->nth_send (7) != 0) {
		current_pot_modes.push_back (Send8);
	}
}

void 
Strip::notify_all()
{
	notify_solo_changed ();
	notify_mute_changed ();
	notify_gain_changed ();
	notify_property_changed (PBD::PropertyChange (ARDOUR::Properties::name));
	notify_panner_changed ();
	notify_record_enable_changed ();
}

void 
Strip::notify_solo_changed ()
{
	if (_route && _solo) {
		_surface->write (_solo->set_state (_route->soloed() ? on : off));
	}
}

void 
Strip::notify_mute_changed ()
{
	DEBUG_TRACE (DEBUG::MackieControl, string_compose ("Strip %1 mute changed\n", _index));
	if (_route && _mute) {
		DEBUG_TRACE (DEBUG::MackieControl, string_compose ("\troute muted ? %1\n", _route->muted()));
		DEBUG_TRACE (DEBUG::MackieControl, string_compose ("mute message: %1\n", _mute->set_state (_route->muted() ? on : off)));

		_surface->write (_mute->set_state (_route->muted() ? on : off));
	}
}

void 
Strip::notify_record_enable_changed ()
{
	if (_route && _recenable)  {
		_surface->write (_recenable->set_state (_route->record_enabled() ? on : off));
	}
}

void 
Strip::notify_active_changed ()
{
	_surface->mcp().refresh_current_bank();
}

void 
Strip::notify_route_deleted ()
{
	_surface->mcp().refresh_current_bank();
}

void 
Strip::notify_gain_changed (bool force_update)
{
	DEBUG_TRACE (DEBUG::MackieControl, string_compose ("gain changed for strip %1, flip mode %2\n", _index, _surface->mcp().flip_mode()));

	if (_route) {
		
		Control* control;

		if (_surface->mcp().flip_mode()) {
			control = _vpot;
		} else {
			control = _fader;
		}

		if (!control->in_use()) {

			boost::shared_ptr<AutomationControl> ac = _route->gain_control();

			float gain_coefficient = ac->get_value();
			float normalized_position = ac->internal_to_interface (gain_coefficient);

			if (force_update || normalized_position != _last_gain_position_written) {


				if (_surface->mcp().flip_mode()) {
					_surface->write (_vpot->set_all (normalized_position, true, Pot::wrap));
					do_parameter_display (GainAutomation, gain_coefficient);
				} else {
					_surface->write (_fader->set_position (normalized_position));
					do_parameter_display (GainAutomation, gain_coefficient);
				}

				queue_display_reset (2000);
				_last_gain_position_written = normalized_position;
				
			} else {
				DEBUG_TRACE (DEBUG::MackieControl, "value is stale, no message sent\n");
			}
		} else {
			DEBUG_TRACE (DEBUG::MackieControl, "fader in use, no message sent\n");
		}
	} else {
		DEBUG_TRACE (DEBUG::MackieControl, "no route or no fader\n");
	}
}

void 
Strip::notify_property_changed (const PropertyChange& what_changed)
{
	if (!what_changed.contains (ARDOUR::Properties::name)) {
		return;
	}

	if (_route) {
		string line1;
		string fullname = _route->name();
		
		if (fullname.length() <= 6) {
			line1 = fullname;
		} else {
			line1 = PBD::short_version (fullname, 6);
		}
		
		_surface->write (display (0, line1));
	}
}

void 
Strip::notify_panner_changed (bool force_update)
{
	if (_route) {

		DEBUG_TRACE (DEBUG::MackieControl, string_compose ("pan change for strip %1\n", _index));

		boost::shared_ptr<Pannable> pannable = _route->pannable();

		if (!pannable) {
			_surface->write (_vpot->zero());
			return;
		}

		Control* control;

		if (_surface->mcp().flip_mode()) {
			control = _fader;
		} else {
			control = _vpot;
		}

		if (!control->in_use()) {
			
			double pos = pannable->pan_azimuth_control->internal_to_interface (pannable->pan_azimuth_control->get_value());
			
			if (force_update || pos != _last_pan_position_written) {
				
				if (_surface->mcp().flip_mode()) {
					
					_surface->write (_fader->set_position (pos));
					do_parameter_display (PanAzimuthAutomation, pos);
				} else {
					_surface->write (_vpot->set_all (pos, true, Pot::dot));
					do_parameter_display (PanAzimuthAutomation, pos);
				}

				queue_display_reset (2000);
				_last_pan_position_written = pos;
			}
		}
	}
}

void
Strip::select_event (Button& button, ButtonState bs)
{
	DEBUG_TRACE (DEBUG::MackieControl, "select button\n");
	
	if (bs == press) {
		
		int ms = _surface->mcp().modifier_state();

		if (ms & MackieControlProtocol::MODIFIER_CMDALT) {
			_controls_locked = !_controls_locked;
			_surface->write (display (1,_controls_locked ?  "Locked" : "Unlock"));
			queue_display_reset (1000);
				return;
		}
		
		if (ms & MackieControlProtocol::MODIFIER_SHIFT) {
			/* reset to default */
			boost::shared_ptr<AutomationControl> ac = _vpot->control ();
			if (ac) {
				ac->set_value (ac->normal());
			}
			return;
		}
		
		DEBUG_TRACE (DEBUG::MackieControl, "add select button on press\n");
		_surface->mcp().add_down_select_button (_surface->number(), _index);			
		_surface->mcp().select_range ();
		
	} else {
		DEBUG_TRACE (DEBUG::MackieControl, "remove select button on release\n");
		_surface->mcp().remove_down_select_button (_surface->number(), _index);			
	}
}

void
Strip::vselect_event (Button& button, ButtonState bs)
{
	if (bs == press) {


		int ms = _surface->mcp().modifier_state();
				
		if (ms & MackieControlProtocol::MODIFIER_SHIFT) {
			boost::shared_ptr<AutomationControl> ac = button.control ();

			if (ac) {
				
				/* reset to default/normal value */
				ac->set_value (ac->normal());
			}

		}  else {
			DEBUG_TRACE (DEBUG::MackieControl, "switching to next pot mode\n");
			next_pot_mode ();
		}

	}
}

void
Strip::fader_touch_event (Button& button, ButtonState bs)
{
	DEBUG_TRACE (DEBUG::MackieControl, string_compose ("fader touch, press ? %1\n", (bs == press)));
	
	/* never use the modified control for fader stuff */
	
	if (bs == press) {
		
		_fader->set_in_use (true);
		_fader->start_touch (_surface->mcp().transport_frame());
		boost::shared_ptr<AutomationControl> ac = _fader->control ();
			if (ac) {
				do_parameter_display ((AutomationType) ac->parameter().type(), ac->internal_to_interface (ac->get_value()));
				queue_display_reset (2000);
			}
			
	} else {
		
		_fader->set_in_use (false);
		_fader->stop_touch (_surface->mcp().transport_frame(), true);
		
	}
}	


void
Strip::handle_button (Button& button, ButtonState bs)
{
	boost::shared_ptr<AutomationControl> control;

	if (bs == press) {
		button.set_in_use (true);
	} else {
		button.set_in_use (false);
	}

	DEBUG_TRACE (DEBUG::MackieControl, string_compose ("strip %1 handling button %2 press ? %3\n", _index, button.bid(), (bs == press)));
	
	switch (button.bid()) {
	case Button::Select:
		select_event (button, bs);
		break;
		
	case Button::VSelect:
		vselect_event (button, bs);
		break;

	case Button::FaderTouch:
		fader_touch_event (button, bs);
		break;

	default:
		if ((control = button.control ())) {
			if (bs == press) {
				DEBUG_TRACE (DEBUG::MackieControl, "add button on press\n");
				_surface->mcp().add_down_button ((AutomationType) control->parameter().type(), _surface->number(), _index);
				
				float new_value;
				int ms = _surface->mcp().modifier_state();
				
				if (ms & MackieControlProtocol::MODIFIER_SHIFT) {
					/* reset to default/normal value */
					new_value = control->normal();
				} else {
					new_value = control->get_value() ? 0.0 : 1.0;
				}
				
				/* get all controls that either have their
				 * button down or are within a range of
				 * several down buttons
				 */
				
				MackieControlProtocol::ControlList controls = _surface->mcp().down_controls ((AutomationType) control->parameter().type());
				
				
				DEBUG_TRACE (DEBUG::MackieControl, string_compose ("there are %1 buttons down for control type %2, new value = %3\n",
									    controls.size(), control->parameter().type(), new_value));

				/* apply change */
				
				for (MackieControlProtocol::ControlList::iterator c = controls.begin(); c != controls.end(); ++c) {
					(*c)->set_value (new_value);
				}
				
			} else {
				DEBUG_TRACE (DEBUG::MackieControl, "remove button on release\n");
				_surface->mcp().remove_down_button ((AutomationType) control->parameter().type(), _surface->number(), _index);
			}
		}
		break;
	}
}

void
Strip::do_parameter_display (AutomationType type, float val)
{
	switch (type) {
	case GainAutomation:
		if (val == 0.0) {
			_surface->write (display (1, " -inf "));
		} else {
			char buf[16];
			float dB = accurate_coefficient_to_dB (val);
			snprintf (buf, sizeof (buf), "%6.1f", dB);
			_surface->write (display (1, buf));
		}		
		break;

	case PanAzimuthAutomation:
		if (_route) {
			boost::shared_ptr<Pannable> p = _route->pannable();
			if (p && p->panner()) {
				string str = p->panner()->value_as_string (p->pan_azimuth_control);
				_surface->write (display (1, str));
			}
		}
	default:
		break;
	}
}

void
Strip::handle_fader (Fader& fader, float position)
{
	DEBUG_TRACE (DEBUG::MackieControl, string_compose ("fader to %1\n", position));

	fader.set_value (position);
	fader.start_touch (_surface->mcp().transport_frame());
	queue_display_reset (2000);

	// must echo bytes back to slider now, because
	// the notifier only works if the fader is not being
	// touched. Which it is if we're getting input.

	_surface->write (fader.set_position (position));
}

void
Strip::handle_pot (Pot& pot, float delta)
{
	/* Pots only emit events when they move, not when they
	   stop moving. So to get a stop event, we need to use a timeout.
	*/
	
	pot.start_touch (_surface->mcp().transport_frame());

	double p = pot.get_value ();
	p += delta;
	p = min (1.0, p);
	p = max (0.0, p);
	pot.set_value (p);
}

void
Strip::periodic (uint64_t usecs)
{
	if (!_route) {
		return;
	}

	update_automation ();
	update_meter ();

	if (_reset_display_at && _reset_display_at < usecs) {
		reset_display ();
	}
}

void 
Strip::update_automation ()
{
	ARDOUR::AutoState gain_state = _route->gain_control()->automation_state();

	if (gain_state == Touch || gain_state == Play) {
		notify_gain_changed (false);
	}

	if (_route->panner()) {
		ARDOUR::AutoState panner_state = _route->panner()->automation_state();
		if (panner_state == Touch || panner_state == Play) {
			notify_panner_changed (false);
		}
	}
}

void
Strip::update_meter ()
{
	if (_meter) {
		float dB = const_cast<PeakMeter&> (_route->peak_meter()).peak_power (0);
		_surface->write (_meter->update_message (dB));
	}
}

MidiByteArray
Strip::zero ()
{
	MidiByteArray retval;

	for (Group::Controls::const_iterator it = _controls.begin(); it != _controls.end(); ++it) {
		retval << (*it)->zero ();
	}

	retval << blank_display (0);
	retval << blank_display (1);
	
	return retval;
}

MidiByteArray
Strip::blank_display (uint32_t line_number)
{
	return display (line_number, string());
}

MidiByteArray
Strip::display (uint32_t line_number, const std::string& line)
{
	assert (line_number <= 1);

	MidiByteArray retval;

	DEBUG_TRACE (DEBUG::MackieControl, string_compose ("strip_display index: %1, line %2 = %3\n", _index, line_number, line));

	// sysex header
	retval << _surface->sysex_hdr();
	
	// code for display
	retval << 0x12;
	// offset (0 to 0x37 first line, 0x38 to 0x6f for second line)
	retval << (_index * 7 + (line_number * 0x38));
	
	// ascii data to display
	retval << line;
	// pad with " " out to 6 chars
	for (int i = line.length(); i < 6; ++i) {
		retval << ' ';
	}
	
	// column spacer, unless it's the right-hand column
	if (_index < 7) {
		retval << ' ';
	}

	// sysex trailer
	retval << MIDI::eox;
	
	DEBUG_TRACE (DEBUG::MackieControl, string_compose ("MackieMidiBuilder::strip_display midi: %1\n", retval));

	return retval;
}

void
Strip::lock_controls ()
{
	_controls_locked = true;
}

void
Strip::unlock_controls ()
{
	_controls_locked = false;
}

MidiByteArray
Strip::gui_selection_changed (ARDOUR::RouteNotificationListPtr rl)
{
	for (ARDOUR::RouteNotificationList::iterator i = rl->begin(); i != rl->end(); ++i) {
		if ((*i) == _route) {
			return _select->set_state (on);
		}
	}

	return _select->set_state (off);
}

string
Strip::vpot_mode_string () const
{
	switch (_vpot_mode) {
	case Gain:
		return "Fader";
	case PanAzimuth:
		return "Pan";
	case PanWidth:
		return "Width";
	case PanElevation:
		return "Elev";
	case PanFrontBack:
		return "F/Rear";
	case PanLFE:
		return "LFE";
	case Input:
		return "Input";
	case Output:
		return "Output";
	case Send1:
		return "Send 1";
	case Send2:
		return "Send 2";
	case Send3:
		return "Send 3";
	case Send4:
		return "Send 4";
	case Send5:
		return "Send 5";
	case Send6:
		return "Send 6";
	case Send7:
		return "Send 7";
	case Send8:
		return "Send 8";
	}

	return "???";
}

void
Strip::flip_mode_changed (bool notify)
{
	if (!_route) {
		return;
	}

	boost::shared_ptr<AutomationControl> fader_controllable = _fader->control ();
	boost::shared_ptr<AutomationControl> vpot_controllable = _vpot->control ();

	_fader->set_control (vpot_controllable);
	_vpot->set_control (fader_controllable);

	_surface->write (display (1, vpot_mode_string ()));

	if (notify) {
		notify_all ();
	}
}

void
Strip::queue_display_reset (uint32_t msecs)
{
	struct timeval now;
	struct timeval delta;
	struct timeval when;
	gettimeofday (&now, 0);
	
	delta.tv_sec = msecs/1000;
	delta.tv_usec = (msecs - ((msecs/1000) * 1000)) * 1000;
	
	timeradd (&now, &delta, &when);

	_reset_display_at = (when.tv_sec * 1000000) + when.tv_usec;
}

void
Strip::clear_display_reset ()
{
	_reset_display_at = 0;
}

void
Strip::reset_display ()
{
	_surface->write (display (1, vpot_mode_string()));
	clear_display_reset ();
}
			 
struct RouteCompareByName {
	bool operator() (boost::shared_ptr<Route> a, boost::shared_ptr<Route> b) {
		return a->name().compare (b->name()) < 0;
	}
};

void
Strip::maybe_add_to_bundle_map (BundleMap& bm, boost::shared_ptr<Bundle> b, bool for_input, const ChanCount& channels)
{
	if (b->ports_are_outputs() == !for_input  || b->nchannels() != channels) {
		return;
	}

	bm[b->name()] = b;
}

void
Strip::build_input_list (const ChanCount& channels)
{
	boost::shared_ptr<ARDOUR::BundleList> b = _surface->mcp().get_session().bundles ();

	input_bundles.clear ();

	/* give user bundles first chance at being in the menu */
	
	for (ARDOUR::BundleList::iterator i = b->begin(); i != b->end(); ++i) {
		if (boost::dynamic_pointer_cast<UserBundle> (*i)) {
			maybe_add_to_bundle_map (input_bundles, *i, true, channels);
		}
	}
	
	for (ARDOUR::BundleList::iterator i = b->begin(); i != b->end(); ++i) {
		if (boost::dynamic_pointer_cast<UserBundle> (*i) == 0) {
			maybe_add_to_bundle_map (input_bundles, *i, true, channels);
		}
	}
	
	boost::shared_ptr<ARDOUR::RouteList> routes = _surface->mcp().get_session().get_routes ();
	RouteList copy = *routes;
	copy.sort (RouteCompareByName ());

	for (ARDOUR::RouteList::const_iterator i = copy.begin(); i != copy.end(); ++i) {
		maybe_add_to_bundle_map (input_bundles, (*i)->output()->bundle(), true, channels);
	}

}

void
Strip::build_output_list (const ChanCount& channels)
{
	boost::shared_ptr<ARDOUR::BundleList> b = _surface->mcp().get_session().bundles ();

	output_bundles.clear ();

	/* give user bundles first chance at being in the menu */
	
	for (ARDOUR::BundleList::iterator i = b->begin(); i != b->end(); ++i) {
		if (boost::dynamic_pointer_cast<UserBundle> (*i)) {
			maybe_add_to_bundle_map (output_bundles, *i, false, channels);
		}
	}
	
	for (ARDOUR::BundleList::iterator i = b->begin(); i != b->end(); ++i) {
		if (boost::dynamic_pointer_cast<UserBundle> (*i) == 0) {
			maybe_add_to_bundle_map (output_bundles, *i, false, channels);
		}
	}
	
	boost::shared_ptr<ARDOUR::RouteList> routes = _surface->mcp().get_session().get_routes ();
	RouteList copy = *routes;
	copy.sort (RouteCompareByName ());

	for (ARDOUR::RouteList::const_iterator i = copy.begin(); i != copy.end(); ++i) {
		maybe_add_to_bundle_map (output_bundles, (*i)->input()->bundle(), false, channels);
	}
}

void
Strip::next_pot_mode ()
{
	vector<PotMode>::iterator i;

	if (_surface->mcp().flip_mode()) {
		/* do not change vpot mode while in flipped mode */
		DEBUG_TRACE (DEBUG::MackieControl, "not stepping pot mode - in flip mode\n");
		_surface->write (display (1, "Flip"));
		queue_display_reset (2000);
		return;
	}

	for (i = current_pot_modes.begin(); i != current_pot_modes.end(); ++i) {
		if ((*i) == _vpot_mode) {
			break;
		}
	}

	/* move to the next mode in the list, or back to the start (which will
	   also happen if the current mode is not in the current pot mode list)
	*/

	if (i != current_pot_modes.end()) {
		++i;
	}

	if (i == current_pot_modes.end()) {
		i = current_pot_modes.begin();
	}

	set_vpot_mode (*i);
}

void
Strip::set_vpot_mode (PotMode m)
{
	boost::shared_ptr<Send> send;
	boost::shared_ptr<Pannable> pannable;

	DEBUG_TRACE (DEBUG::MackieControl, string_compose ("switch to vpot mode %1\n", m));

	if (!_route) {
		return;
	}

	_vpot_mode = m;

	switch (_vpot_mode) {
	case Gain:
		break;
	case PanAzimuth:
		pannable = _route->pannable ();
		if (pannable) {
			if (_surface->mcp().flip_mode()) {
				/* gain to vpot, pan azi to fader */
				_vpot->set_control (_route->gain_control());
				if (pannable) {
					_fader->set_control (pannable->pan_azimuth_control);
				}
				_vpot_mode = Gain;
			} else {
				/* gain to fader, pan azi to vpot */
				_fader->set_control (_route->gain_control());
				if (pannable) {
					_vpot->set_control (pannable->pan_azimuth_control);
				}
			}
		}
		break;
	case PanWidth:
		pannable = _route->pannable ();
		if (pannable) {
			if (_surface->mcp().flip_mode()) {
				/* gain to vpot, pan width to fader */
				_vpot->set_control (_route->gain_control());
				if (pannable) {
					_fader->set_control (pannable->pan_width_control);
				}
				_vpot_mode = Gain;
			} else {
				/* gain to fader, pan width to vpot */
				_fader->set_control (_route->gain_control());
				if (pannable) {
					_vpot->set_control (pannable->pan_width_control);
				}
			}
		}
		break;
	case PanElevation:
		break;
	case PanFrontBack:
		break;
	case PanLFE:
		break;
	case Input:
		break;
	case Output:
		break;
	case Send1:
		send = boost::dynamic_pointer_cast<Send> (_route->nth_send (0));
		if (send) {
			if (_surface->mcp().flip_mode()) {
				/* route gain to vpot, send gain to fader */
				_fader->set_control (send->amp()->gain_control());
				_vpot->set_control (_route->gain_control());
				_vpot_mode = Gain;
				} else {
				/* route gain to fader, send gain to vpot */
				_vpot->set_control (send->amp()->gain_control());
				_fader->set_control (_route->gain_control());
			}
		}
		break;
	case Send2:
		break;
	case Send3:
		break;
	case Send4:
		break;
	case Send5:
		break;
	case Send6:
		break;
	case Send7:
		break;
	case Send8:
		break;
	};

	_surface->write (display (1, vpot_mode_string()));
}
