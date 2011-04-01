#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/wait.h>
#include <unistd.h>

#include "chaosvpn.h"

static bool tinc_add_subnet(struct string*, struct list_head*);

#define CONCAT(buffer, value)	if (string_concat(buffer, value)) return false
#define CONCAT_F(buffer, format, value)	if (string_concat_sprintf(buffer, format, value)) return false
#define CONCAT_DF(buffer, format, value, default_value)	if (string_concat_sprintf(buffer, format, str_is_nonempty(value) ? value : default_value)) return false
#define CONCAT_YN(buffer, format, value, default_value)    if (string_concat_sprintf(buffer, format, str_is_true(value, default_value) ? "yes" : "no")) return false
#define CONCAT_SN(buffer, value)	if (!tinc_add_subnet(buffer, value)) return false

static bool
tinc_check_if_excluded(struct config *config, char *peername)
{
	struct list_head* ptr;
	struct settings_list* etr;

	if (config->exclude == NULL) {
		return false;
	}

	list_for_each(ptr, &config->exclude->list) {
		etr = list_entry(ptr, struct settings_list, list);
		if (etr->e->etype != LIST_STRING) {
			/* only strings allowed */
			continue;
		}
		if (strcasecmp(etr->e->evalue.s, peername) == 0) {
			return true;
		}
	}
	
	return false;
}

static bool
tinc_generate_peer_config(struct string* buffer, struct peer_config *peer)
{
	CONCAT(buffer, "# this is an autogenerated file - do not edit!\n\n");

	if (str_is_nonempty(peer->gatewayhost)) {
		CONCAT_F(buffer, "Address=%s\n", peer->gatewayhost);
	}

	CONCAT_DF(buffer, "Cipher=%s\n", peer->cipher, TINC_DEFAULT_CIPHER);
	CONCAT_DF(buffer, "Compression=%s\n", peer->compression, TINC_DEFAULT_COMPRESSION);
	CONCAT_DF(buffer, "Digest=%s\n", peer->digest, TINC_DEFAULT_DIGEST);
	CONCAT_YN(buffer, "IndirectData=%s\n", peer->indirectdata, false);
	CONCAT_DF(buffer, "Port=%s\n", peer->port, TINC_DEFAULT_PORT);

	CONCAT_SN(buffer, &peer->network);
	CONCAT_SN(buffer, &peer->network6);

	CONCAT_YN(buffer, "TCPonly=%s\n", peer->use_tcp_only, false);
	CONCAT_F(buffer, "%s\n", peer->key);	

	return true;
}

bool
tinc_write_hosts(struct config *config)
{
	struct list_head *p = NULL;
	struct string hostfilepath;

	string_init(&hostfilepath, 512, 512);
	string_concat(&hostfilepath, config->base_path);
	string_concat(&hostfilepath, "/hosts/");

	fs_mkdir_p(string_get(&hostfilepath), 0700);

	list_for_each(p, &config->peer_config) {
		struct string peer_config;
		struct peer_config_list *i = container_of(p, 
				struct peer_config_list, list);

		log_debug("Writing config file for peer %s", i->peer_config->name);
		(void)fflush(stdout);

		if (string_init(&peer_config, 2048, 512)) return false;

		if (!tinc_generate_peer_config(&peer_config, i->peer_config)) {
			string_free(&peer_config);
			return false;
		}

		if (fs_writecontents_safe(string_get(&hostfilepath), 
				i->peer_config->name, string_get(&peer_config),
				string_length(&peer_config), 0600)) {
			log_err("unable to write host config file %s/%s.", string_get(&hostfilepath), i->peer_config->name);
			string_free(&peer_config);
			return false;
		}

		string_free(&peer_config);
	}

	string_free(&hostfilepath);
	
	return true;
}

bool
tinc_write_config(struct config *config)
{
	struct list_head *p;
	struct string configfilename;
	struct string buffer;

	log_debug("Writing global config file.");

	string_init(&buffer, 8192, 2048);


	/* generate main tinc.conf */

	CONCAT(&buffer, "# this is an autogenerated file - do not edit!\n\n");

	CONCAT(&buffer, "AddressFamily=ipv4\n");

	if (str_is_nonempty(config->tincd_interface)) {
		CONCAT_F(&buffer, "Interface=%s\n", config->tincd_interface);
	}
	if (str_is_nonempty(config->tincd_device)) {
		CONCAT_F(&buffer, "Device=%s\n", config->tincd_device);
	}

	CONCAT(&buffer, "Mode=router\n");
	CONCAT_F(&buffer, "Name=%s\n", config->peerid);
	CONCAT(&buffer, "Hostnames=no\n");
	CONCAT(&buffer, "PingTimeout=60\n");

	if (config->tincd_version &&
		(strnatcmp(config->tincd_version, "1.0.12") > 0)) {
		/* this option is only available since 1.0.12+git / 1.0.13 */
		CONCAT(&buffer, "StrictSubnets=yes\n");
	} else {
		CONCAT(&buffer, "TunnelServer=yes\n");
	}

	if (str_is_nonempty(config->tincd_graphdumpfile)) {
		CONCAT_F(&buffer, "GraphDumpFile=%s\n", config->tincd_graphdumpfile);
	}

	if (config->my_ip &&
			str_is_nonempty(config->my_ip) && 
			strcmp(config->my_ip, "127.0.0.1") &&
			strcmp(config->my_ip, "0.0.0.0")) {
		CONCAT_F(&buffer, "BindToAddress=%s\n", config->my_ip);
	}

	if (str_is_true(config->my_peer->silent, false)) {
			return true; //no ConnectTo lines
	}

	list_for_each(p, &config->peer_config) {
		struct peer_config_list *i = container_of(p, struct peer_config_list, list);

		if (!strcmp(i->peer_config->name, config->peerid)) {
			continue;
		}

		if (tinc_check_if_excluded(config, i->peer_config->name)) {
			continue;
		}

		if (config->connect_only_to_primary_nodes &&
				config->tincd_version &&
				(strnatcmp(config->tincd_version, "1.0.12") > 0) &&
				(!str_is_true(i->peer_config->primary, false))) {
			/* tinc 1.0.12+git++ - only connect to primary hosts */
			/* tinc peer2peer will do the rest for us */
			/* this reduces the number of tcp connections */
			/* not enabled by default (yet) because old tinc */
			/* nodes with TunnelServer=yes don't like it */
			continue;
		}
		
		if (str_is_nonempty(i->peer_config->gatewayhost) &&
				!str_is_true(i->peer_config->hidden, false)) {
			CONCAT_F(&buffer, "ConnectTo=%s\n", i->peer_config->name);
		}
	}


	/* write tinc.conf */

	string_init(&configfilename, 512, 512);
	string_concat(&configfilename, config->base_path);
	string_concat(&configfilename, "/tinc.conf");

	if (fs_writecontents(string_get(&configfilename), string_get(&buffer),
			string_length(&buffer), 0600)) {
		log_err("unable to write tinc config file!");
		string_free(&buffer);
		string_free(&configfilename);
		return false;
	}

	string_free(&buffer);
	string_free(&configfilename);

	return true;
}

bool
tinc_write_updown(struct config *config, bool up)
{
	/* up == true:  generate tinc-up */
	/* up == false: generate tinc-down */

	struct list_head *p;
	struct list_head *sp;
	struct peer_config_list *i;
	struct string_list *si;
	struct string buffer;
	struct string filepath;
	const char *routecmd;
	char *subnet;
	char *weight;

	/* generate contents */

	string_init(&buffer, 8192, 2048);
	CONCAT(&buffer, "#!/bin/sh\n");
	CONCAT(&buffer, "# this is an autogenerated file - do not edit!\n\n");

	if (up) {
		if (str_is_nonempty(config->ifconfig) && str_is_nonempty(config->vpn_ip)) {
			CONCAT_F(&buffer, "%s\n", config->ifconfig);
		}

		if (str_is_nonempty(config->ifconfig6) && config->vpn_ip6 && str_is_nonempty(config->vpn_ip6)) {
			CONCAT_F(&buffer, "%s\n", config->ifconfig6);
		}

		CONCAT(&buffer, "\n");
	}

	if (!config->use_dynamic_routes) {
		/* setup / remove all routes unless using dynamic routes */

		list_for_each(p, &config->peer_config) {
			i = container_of(p, struct peer_config_list, list);

			if (!strcmp(i->peer_config->name, config->peerid)) {
				continue;
			}

			if (tinc_check_if_excluded(config, i->peer_config->name)) {
				CONCAT_F(&buffer, "# excluded node: %s\n", i->peer_config->name);
				continue;
			}

			CONCAT_F(&buffer, "# node: %s\n", i->peer_config->name);

			if (up)
				routecmd = config->routeadd;
			else
				routecmd = config->routedel;
			if (str_is_nonempty(config->vpn_ip) && str_is_nonempty(routecmd)) {
				list_for_each(sp, &i->peer_config->network) {
					si = container_of(sp, struct string_list, list);
					subnet = strdup(si->text);
					weight = strchr(subnet, '#');
					if (weight)
						*weight++ = 0;
					CONCAT_F(&buffer, routecmd, subnet);
					CONCAT(&buffer, "\n");
					free(subnet);
				}
			}
			
			if (up)
				routecmd = config->routeadd6;
			else
				routecmd = config->routedel6;
			if (str_is_nonempty(config->vpn_ip6) && str_is_nonempty(routecmd)) {
				list_for_each(sp, &i->peer_config->network6) {
					si = container_of(sp, struct string_list, list);
					subnet = strdup(si->text);
					weight = strchr(subnet, '#');
					if (weight)
						*weight++ = 0;
					CONCAT_F(&buffer, routecmd, subnet);
					CONCAT(&buffer, "\n");
					free(subnet);
				}
			}
		}
	}



	CONCAT(&buffer, "\n");
	CONCAT(&buffer, "[ -x \"$0.local\" ] && \"$0.local\" \"$@\"\n");
	CONCAT(&buffer, "\n");
	if (up) {
		if (config->postup &&
				str_is_nonempty(config->postup)) {
			CONCAT(&buffer, config->postup);
			CONCAT(&buffer, "\n");
		}
	}


	CONCAT(&buffer, "\nexit 0\n\n");


	/* write contents into file */

	string_init(&filepath, 512, 512);
	string_concat(&filepath, config->base_path);
	if (up)
		string_concat(&filepath, "/tinc-up");
	else
		string_concat(&filepath, "/tinc-down");
	if (fs_writecontents(string_get(&filepath), string_get(&buffer), string_length(&buffer), 0700)) {
		log_err("unable to write to %s!", string_get(&filepath));
		string_free(&buffer);
		string_free(&filepath);
		return false;
	}
	
	string_free(&buffer);
	string_free(&filepath);

	return true;
}

bool
tinc_write_subnetupdown(struct config *config, bool up)
{
	/* up == true:  generate subnet-up */
	/* up == false: generate subnet-down */

	struct string buffer;
	struct string filepath;
	const char *routecmd;
	const char *routecmd6;
	const char *logger;

	string_init(&filepath, 512, 512);
	string_concat(&filepath, config->base_path);
	if (up)
		string_concat(&filepath, "/subnet-up");
	else
		string_concat(&filepath, "/subnet-down");


	/* if not in use_dynamic_routes mode DELETE destination file instead! */
	if (!config->use_dynamic_routes) {
		struct string localpath;

		unlink(string_get(&filepath)); /* unlink first */

		/* if subnet-(up|down).local exist symlink it to subnet-(up|down) */
		string_init(&localpath, 512, 512);
		string_concat_sprintf(&localpath, "%s.local", string_get(&filepath));

		if (access(string_get(&localpath), X_OK) == 0) {
			symlink(string_get(&localpath), string_get(&filepath));
		}
		
		string_free(&localpath);
		string_free(&filepath);
		return true;
	}


	/* generate contents */
	
	string_init(&buffer, 8192, 2048);
	CONCAT(&buffer, "#!/bin/sh\n");
	CONCAT(&buffer, "# this is an autogenerated file - do not edit!\n\n");

	if (up) {
		routecmd = config->routeadd;
		routecmd6 = config->routeadd6;
		logger = "logger -t \"tinc.$NETNAME.subnet-up\" -p daemon.debug \"subnet-up from $NODE for %s $SUBNET ($REMOTEADDRESS:$REMOTEPORT)%s\" 2>/dev/null\n";
	} else {
		routecmd = config->routedel;
		routecmd6 = config->routedel6;
		logger = "logger -t \"tinc.$NETNAME.subnet-down\" -p daemon.debug \"subnet-down from $NODE for %s $SUBNET ($REMOTEADDRESS:$REMOTEPORT)%s\" 2>/dev/null\n";
	}

	CONCAT_F(&buffer, "[ \"$NODE\" = '%s' ] && exit 0\n\n", config->peerid);

	/* Create code to check excludes */
	if (config->exclude != NULL) {
		struct list_head* ptr;
		struct settings_list* etr;
		bool haveexcludes = false;

		list_for_each(ptr, &config->exclude->list) {
			etr = list_entry(ptr, struct settings_list, list);
			if (etr->e->etype != LIST_STRING) {
				/* only strings allowed */
				continue;
			}

			if (!haveexcludes) {
				/* first exclude, write prefix */
				
				CONCAT(&buffer, "\n");
				CONCAT(&buffer, "excluded=\"\"\n");
				
				haveexcludes = true;
			}
			
			CONCAT_F(&buffer, "[ \"$NODE\" = '%s' ] && excluded=1\n", etr->e->evalue.s);

		}
		
		if (haveexcludes) {
			/* at least one exclude in config, write exclude footer */

			CONCAT(&buffer, "if [ -n \"$excluded\" ] ; then\n");
			CONCAT(&buffer, "\t");
			if (string_concat_sprintf(&buffer, logger, "ignore", " (excluded)")) return false;
			CONCAT(&buffer, "\t[ -x \"$0.local\" ] && \"$0.local\" \"$@\"\n");
			CONCAT(&buffer, "\texit 0\n");
			CONCAT(&buffer, "fi\n\n");
		}
	}

	if (str_is_nonempty(config->vpn_ip) && str_is_nonempty(routecmd)) {
		CONCAT(&buffer, "if echo \"$SUBNET\" | grep -q '^[0-9]\\+\\.[0-9]\\+\\.[0-9]\\+\\.[0-9]\\+/[0-9]\\+$' ; then\n");
		CONCAT(&buffer, "\t");
		if (string_concat_sprintf(&buffer, logger, "ipv4", "")) return false;
		
		CONCAT(&buffer, "\t");
		CONCAT_F(&buffer, routecmd, "$SUBNET");
		CONCAT(&buffer, "\n");
		CONCAT(&buffer, "\t[ -x \"$0.local\" ] && \"$0.local\" \"$@\"\n");
		CONCAT(&buffer, "\texit 0\n");

		CONCAT(&buffer, "fi\n");
	} else {
		CONCAT(&buffer, "if echo \"$SUBNET\" | grep -q '^[0-9]\\+\\.[0-9]\\+\\.[0-9]\\+\\.[0-9]\\+/[0-9]\\+$' ; then\n");
		CONCAT(&buffer, "\t");
		if (string_concat_sprintf(&buffer, logger, "ipv4", " (disabled)")) return false;
		CONCAT(&buffer, "\t[ -x \"$0.local\" ] && \"$0.local\" \"$@\"\n");
		CONCAT(&buffer, "\texit 0\n");

		CONCAT(&buffer, "fi\n");
	}

	if (str_is_nonempty(config->vpn_ip6) && str_is_nonempty(routecmd6)) {
		CONCAT(&buffer, "if echo \"$SUBNET\" | grep -q -i '^[0-9a-f]\\+:[0-9a-f]\\+:[0-9a-f]\\+:[0-9a-f]\\+:[0-9a-f]\\+:[0-9a-f]\\+:[0-9a-f]\\+:[0-9a-f]\\+/[0-9]\\+$' ; then\n");
		CONCAT(&buffer, "\t");
		if (string_concat_sprintf(&buffer, logger, "ipv6", "")) return false;

		CONCAT(&buffer, "\t");
		CONCAT_F(&buffer, routecmd6, "$SUBNET");
		CONCAT(&buffer, "\n");
		CONCAT(&buffer, "\t[ -x \"$0.local\" ] && \"$0.local\" \"$@\"\n");
		CONCAT(&buffer, "\texit 0\n");

		CONCAT(&buffer, "fi\n");
	} else {
		CONCAT(&buffer, "if echo \"$SUBNET\" | grep -q -i '^[0-9a-f]\\+:[0-9a-f]\\+:[0-9a-f]\\+:[0-9a-f]\\+:[0-9a-f]\\+:[0-9a-f]\\+:[0-9a-f]\\+:[0-9a-f]\\+/[0-9]\\+$' ; then\n");
		CONCAT(&buffer, "\t");
		if (string_concat_sprintf(&buffer, logger, "ipv6", " (disabled)")) return false;
		CONCAT(&buffer, "\t[ -x \"$0.local\" ] && \"$0.local\" \"$@\"\n");
		CONCAT(&buffer, "\texit 0\n");

		CONCAT(&buffer, "fi\n");
	}

	if (string_concat_sprintf(&buffer, logger, "unknown", " (ignored)")) return false;
	CONCAT(&buffer, "[ -x \"$0.local\" ] && \"$0.local\" \"$@\"\n");
	CONCAT(&buffer, "exit 0\n\n");


	/* write contents into file */
	
	unlink(string_get(&filepath)); /* unlink first, may be a symlink */

	if (fs_writecontents(string_get(&filepath), string_get(&buffer), string_length(&buffer), 0700)) {
		log_err("unable to write to %s!\n", string_get(&filepath));
		string_free(&buffer);
		string_free(&filepath);
		return false;
	}

	string_free(&buffer);
	string_free(&filepath);
	
	return true;
}

static bool
tinc_add_subnet(struct string* buffer, struct list_head *network)
{
	struct list_head *p;

	list_for_each(p, network) {
		struct string_list *i = container_of(p, struct string_list, list);
		CONCAT_F(buffer, "Subnet=%s\n", i->text);
	}

	return true;
}

char *
tinc_get_version(struct config *config)
{
	struct string tincd_output;
	char *retval = NULL;
	char cmd[1024];
	char *p;
	int res;
	
	string_init(&tincd_output, 1024, 512);
	snprintf(cmd, sizeof(cmd), "%s --version", config->tincd_bin);
	res = fs_backticks_exec(cmd, &tincd_output);
	if (string_putc(&tincd_output, 0)) goto bail_out;

	if (strncmp(string_get(&tincd_output), "tinc version ", 13) != 0) {
		retval = NULL;
		goto bail_out;
	}

	if ((p = strchr(string_get(&tincd_output)+13, ' '))) {
		*p = '\0';
	}
	
	//log_debug("tinc version output: '%s'\n", string_get(&tincd_output));

	retval = strdup(string_get(&tincd_output)+13);

	//log_debug("tinc version: '%s'\n", retval);

bail_out:
	string_free(&tincd_output);
	return retval;
}

pid_t
tinc_get_pid(struct config *config)
{
	struct string pid_text;
	pid_t pid = 0;
	char cmd[1024];
	int res;

	string_init(&pid_text, 256, 128);

	if (config->tincd_version &&
		(strnatcmp(config->tincd_version, "1.1") > 0)) {
		/* tinc 1.1-git development tree does not use pid files anymore */

		if (str_is_empty(config->tincctl_bin))
			goto bail_out;

		snprintf(cmd, sizeof(cmd), "%s --net=%s pid 2>/dev/null", config->tincctl_bin, config->networkname);
		res = fs_backticks_exec(cmd, &pid_text);
	} else {
		/* tinc 1.0.x - use pid file */

		if (str_is_empty(config->pidfile)) {
			log_warn("Notice: tinc pidfile not specified!");
			goto bail_out;
		}

		if (fs_read_file(&pid_text, config->pidfile)) {
			log_info("Notice: unable to open pidfile '%s'; assuming an old tincd is not running", config->pidfile);
			goto bail_out;
		}
	}

	/* NULL terminate string */
	if (string_putc(&pid_text, 0)) goto bail_out;

	if (str_is_empty(string_get(&pid_text))) {
		log_info("Notice: unable to find tinc pid; assuming an old tincd is not running");
		goto bail_out;
	}

	res = strtol(string_get(&pid_text), NULL, 10);
	pid = (pid_t) res;

bail_out:
	string_free(&pid_text);
	return pid;
}

bool
tinc_invoke_ifdown(struct config* config)
{
	pid_t fd;
	int status;
	int i;
	struct string filepath;

	if (!config->run_ifdown) return true;

	string_init(&filepath, 512, 512);
	string_concat(&filepath, config->base_path);
	string_concat(&filepath, "/tinc-down");
	string_ensurez(&filepath);
	
	switch((fd=fork())) {
	case 0:
		for(i=3;i<65536;i++) close(i);
		log_debug("Exec'ing to %s", string_get(&filepath));
		execl(string_get(&filepath), string_get(&filepath), NULL);
		_exit(1);
	case -1:
		log_err("Unable to invoke tinc-down script");
		return false;
	default:
		waitpid(fd, &status, 0);
		if (status != 0) {
			log_err("tinc-down failed");
			return false;
		}
		return true;
	}
}
