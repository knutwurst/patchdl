#pragma once

/* Send an on-screen PS5 notification at startup showing the web UI URL
   (with the console's LAN IP when resolvable). */
void patchdl_notify_startup(unsigned short port);
