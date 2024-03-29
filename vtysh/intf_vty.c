/* Interface CLI commands
 *
 * Copyright (C) 1997, 98 Kunihiro Ishiguro
 * Copyright (C) 2015 Hewlett Packard Enterprise Development LP
 *
 * GNU Zebra is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Zebra; see the file COPYING.  If not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * File: intf_vty.c
 *
 * Purpose:  To add Interface/Port related configuration and display commands.
 */

#include <sys/un.h>
#include <setjmp.h>
#include <sys/wait.h>
#include <pwd.h>

#include <readline/readline.h>
#include <readline/history.h>

#include <lib/version.h>
#include "getopt.h"
#include "command.h"
#include "memory.h"
#include "vtysh/vtysh.h"
#include "vtysh/vtysh_user.h"
#include "vswitch-idl.h"
#include "ovsdb-idl.h"
#include "intf_vty.h"
#include "lacp_vty.h"
#include "smap.h"
#include "openvswitch/vlog.h"
#include "openswitch-idl.h"
#include "vtysh/vtysh_ovsdb_if.h"
#include "vtysh/vtysh_ovsdb_config.h"
#include "vtysh/mgmt_intf_vty.h"
#include "vtysh/vtysh_ovsdb_intf_context.h"
#include "lacp_vty.h"

VLOG_DEFINE_THIS_MODULE(vtysh_interface_cli);
extern struct ovsdb_idl *idl;

#define INTF_NAME_SIZE 50


/* qsort comparator function.
 * This may need to be modified depending on the format of interface name
 * Currently interface name format is[interface_number-split interface_number]
 */
static int
compare_nodes_by_interface_in_numerical(const void *a_, const void *b_)
{
    const struct shash_node *const *a = a_;
    const struct shash_node *const *b = b_;
    uint i1=0,i2=0,i3=0,i4=0;

    sscanf((*a)->name,"%d-%d",&i1,&i2);
    sscanf((*b)->name,"%d-%d",&i3,&i4);

    if(i1 == i3)
    {
        if(i2 == i4)
            return 0;
        else if(i2 < i4)
            return -1;
        else
            return 1;
    }
    else if (i1 < i3)
        return -1;
    else
        return 1;

    return 0;
}

/*
 * Sorting function for interface
 * on success, returns sorted interface list.
 */
static const struct shash_node **
sort_interface(const struct shash *sh)
{
    if (shash_is_empty(sh)) {
        return NULL;
    } else {
        const struct shash_node **nodes;
        struct shash_node *node;

        size_t i, n;

        n = shash_count(sh);
        nodes = xmalloc(n * sizeof *nodes);
        i = 0;
        SHASH_FOR_EACH (node, sh) {
            nodes[i++] = node;
        }
        ovs_assert(i == n);

        qsort(nodes, n, sizeof *nodes, compare_nodes_by_interface_in_numerical);
        return nodes;
    }
}


/*
 * CLI "shutdown"
 * default : enabled
 */
DEFUN (cli_intf_shutdown,
        cli_intf_shutdown_cmd,
        "shutdown",
        "Enable/disable an interface\n")
{
    const struct ovsrec_interface * row = NULL;
    const struct ovsrec_port *port_row = NULL;
    struct ovsdb_idl_txn* status_txn = cli_do_config_start();
    enum ovsdb_idl_txn_status status;
    struct smap smap_user_config;

    if (status_txn == NULL)
    {
        VLOG_ERR(OVSDB_TXN_CREATE_ERROR);
        cli_do_config_abort(status_txn);
        return CMD_OVSDB_FAILURE;
    }

    row = ovsrec_interface_first(idl);
    if (!row)
    {
        cli_do_config_abort(status_txn);
        return CMD_OVSDB_FAILURE;
    }

    OVSREC_INTERFACE_FOR_EACH(row, idl)
    {
        if (strcmp(row->name, (char*)vty->index) == 0)
        {
            smap_clone(&smap_user_config, &row->user_config);

            if (vty_flags & CMD_FLAG_NO_CMD)
            {
                smap_replace(&smap_user_config,
                        INTERFACE_USER_CONFIG_MAP_ADMIN,
                        OVSREC_INTERFACE_USER_CONFIG_ADMIN_UP);
            }
            else
            {
                smap_remove(&smap_user_config,
                        INTERFACE_USER_CONFIG_MAP_ADMIN);
            }
            ovsrec_interface_set_user_config(row, &smap_user_config);
            break;
        }
    }

    OVSREC_PORT_FOR_EACH(port_row, idl)
    {
        if(strcmp(port_row->name, (char*)vty->index) == 0)
        {
            if(vty_flags & CMD_FLAG_NO_CMD)
            {
                ovsrec_port_set_admin(port_row,
                        OVSREC_INTERFACE_ADMIN_STATE_UP);
            }
            else
            {
                ovsrec_port_set_admin(port_row,
                        OVSREC_INTERFACE_ADMIN_STATE_DOWN);
            }
            break;
        }
    }

    status = cli_do_config_finish(status_txn);

    smap_destroy(&smap_user_config);

    if (status == TXN_SUCCESS || status == TXN_UNCHANGED)
    {
        return CMD_SUCCESS;
    }
    else
    {
        VLOG_ERR(OVSDB_TXN_COMMIT_ERROR);
    }

    return CMD_OVSDB_FAILURE;
}

DEFUN_NO_FORM (cli_intf_shutdown,
        cli_intf_shutdown_cmd,
        "shutdown",
        "Enable/disable an interface\n");


void
dyncb_helpstr_speeds(struct cmd_token *token, struct vty *vty, \
                     char * const helpstr, int max_strlen)
{
    const struct ovsrec_interface * row = NULL;
    const char *speeds_list = NULL;
    char * tmp = NULL;
    int len = 0;
    row = ovsrec_interface_first(idl);
    if (!row)
    {
        VLOG_ERR(OVSDB_ROW_FETCH_ERROR);
        return NULL;
    }

    OVSREC_INTERFACE_FOR_EACH(row, idl)
    {
        if(strcmp(row->name, vty->index) != 0)
            continue;

        speeds_list = smap_get(&row->hw_intf_info, "speeds");
        if (speeds_list != NULL)
        {
            char * cur_state = calloc(strlen(speeds_list) + 1, sizeof(char));
            strcpy(cur_state, speeds_list);
            tmp = strtok(cur_state, ",");
            while (tmp != NULL)
            {
                if (strcmp(tmp, token->cmd) == 0)
                {
                    snprintf(helpstr, max_strlen, "Mb/s supported");
                    free(cur_state);
                    return;
                }
                tmp = strtok(NULL, ",");
            }
            snprintf(helpstr, max_strlen, "Mb/s not supported");
            free(cur_state);
        }
        else
            snprintf(helpstr, max_strlen, "Mb/s not configured");
    }
    return;
}

/*
 * CLI "speed"
 * default : auto
 * Maximum speed is consitent with maximum speed mentioned in ports.yaml.
 */
DEFUN_DYN_HELPSTR (cli_intf_speed,
      cli_intf_speed_cmd,
      "speed (auto|1000|10000|40000)",
      "Configure the interface speed\n"
      "Auto negotiate speed (Default)\n"
      "1Gb/s\n10Gb/s\n40Gb/s",
      "\n\ndyncb_helpstr_1G\ndyncb_helpstr_10G\ndyncb_helpstr_40G")
{
    const struct ovsrec_interface * row = NULL;
    struct ovsdb_idl_txn* status_txn = cli_do_config_start();
    enum ovsdb_idl_txn_status status;
    struct smap smap_user_config;
    const char *speeds_list = NULL;
    char *tmp = NULL;
    int support_flag = 0;
    char *cur_state = NULL;

    if (status_txn == NULL)
    {
        VLOG_ERR(OVSDB_TXN_CREATE_ERROR);
        cli_do_config_abort(status_txn);
        return CMD_OVSDB_FAILURE;
    }

    row = ovsrec_interface_first(idl);
    if (!row)
    {
        cli_do_config_abort(status_txn);
        return CMD_OVSDB_FAILURE;
    }

    OVSREC_INTERFACE_FOR_EACH(row, idl)
    {
        if (strcmp(row->name, (char*)vty->index) == 0)
        {
            smap_clone(&smap_user_config,&row->user_config);

            if (vty_flags & CMD_FLAG_NO_CMD)
            {
                smap_remove(&smap_user_config,
                        INTERFACE_USER_CONFIG_MAP_SPEEDS);
            }
            else
            {
                if (strcmp(INTERFACE_USER_CONFIG_MAP_SPEEDS_DEFAULT,
                        argv[0]) == 0)
                {
                    smap_remove(&smap_user_config,
                            INTERFACE_USER_CONFIG_MAP_SPEEDS);
                }
                else
                {
                    speeds_list = smap_get(&row->hw_intf_info, "speeds");
                    support_flag = 0;
                    if (speeds_list != NULL)
                    {
                        cur_state = calloc(strlen(speeds_list) + 1,
                                                           sizeof(char));
                        strcpy(cur_state, speeds_list);
                        tmp = strtok(cur_state, ",");
                        while (tmp != NULL)
                        {
                            if (strcmp(tmp, argv[0]) == 0)
                            {
                                support_flag = 1;
                                break;
                            }
                            tmp = strtok(NULL, ",");
                        }
                        free(cur_state);
                        if (support_flag == 0)
                        {
                            vty_out(vty, "Interface doesn't support %s (Mb/s). "
                                        "Supported speed(s) : %s (Mb/s).%s",
                                        argv[0], speeds_list, VTY_NEWLINE);
                            cli_do_config_abort(status_txn);
                            smap_destroy(&smap_user_config);
                            return CMD_SUCCESS;
                         }
                            smap_replace(&smap_user_config,
                                         INTERFACE_USER_CONFIG_MAP_SPEEDS,
                                         argv[0]);
                      }
                }
            }
            ovsrec_interface_set_user_config(row, &smap_user_config);
            break;
        }
    }

    status = cli_do_config_finish(status_txn);

    smap_destroy(&smap_user_config);

    if (status == TXN_SUCCESS || status == TXN_UNCHANGED)
    {
        return CMD_SUCCESS;
    }
    else
    {
        VLOG_ERR(OVSDB_TXN_COMMIT_ERROR);
    }

    return CMD_OVSDB_FAILURE;
}

DEFUN_NO_FORM (cli_intf_speed,
        cli_intf_speed_cmd,
        "speed",
        "Enter the interface speed\n");

void
dyncb_helpstr_mtu(struct cmd_token *token, struct vty *vty, \
                  char * const helpstr, int max_strlen)
{
    const struct ovsrec_subsystem * row = NULL;
    char * mtu = NULL;

    row = ovsrec_subsystem_first(idl);
    if (!row)
    {
        VLOG_ERR(OVSDB_ROW_FETCH_ERROR);
        return NULL;
    }

    OVSREC_SUBSYSTEM_FOR_EACH(row, idl)
    {
        mtu = smap_get(&row->other_info, "max_transmission_unit");
        if (mtu != NULL)
            snprintf(helpstr, max_strlen, \
                     "Enter MTU (in bytes) in the range <576-%s>", mtu);
    }
    return;
}

/*
 * CLI "mtu"
 * default : auto
 */
DEFUN_DYN_HELPSTR (cli_intf_mtu,
        cli_intf_mtu_cmd,
        "mtu (auto|WORD)",
        "Configure MTU for the interface\n"
        "Set MTU to system default (Default)\n"
        "Enter MTU (in bytes) in the range <576-9216>\n",
        "\n\ndyncb_helpstr_mtu\n")
{
    const struct ovsrec_interface * row = NULL;
    struct ovsdb_idl_txn* status_txn = cli_do_config_start();
    enum ovsdb_idl_txn_status status;
    struct smap smap_user_config;

    if (status_txn == NULL)
    {
        VLOG_ERR(OVSDB_TXN_CREATE_ERROR);
        cli_do_config_abort(status_txn);
        return CMD_OVSDB_FAILURE;
    }

    row = ovsrec_interface_first(idl);
    if (!row)
    {
        cli_do_config_abort(status_txn);
        return CMD_OVSDB_FAILURE;
    }

    OVSREC_INTERFACE_FOR_EACH(row, idl)
    {
        if (strcmp(row->name, (char*)vty->index) == 0)
        {
            smap_clone(&smap_user_config, &row->user_config);

            if (vty_flags & CMD_FLAG_NO_CMD)
            {
                smap_remove(&smap_user_config, INTERFACE_USER_CONFIG_MAP_MTU);
            }
            else
            {
                if (strcmp(INTERFACE_USER_CONFIG_MAP_MTU_DEFAULT,
                        argv[0]) == 0)
                {
                    smap_remove(&smap_user_config,
                            INTERFACE_USER_CONFIG_MAP_MTU);
                }
                else
                {
                    smap_replace(&smap_user_config,
                            INTERFACE_USER_CONFIG_MAP_MTU, argv[0]);
                }
            }
            ovsrec_interface_set_user_config(row, &smap_user_config);
            break;
        }
    }

    status = cli_do_config_finish(status_txn);

    if (status == TXN_SUCCESS || status == TXN_UNCHANGED)

        smap_destroy(&smap_user_config);

    if (status == TXN_SUCCESS || status == TXN_UNCHANGED)
    {
        return CMD_SUCCESS;
    }
    else
    {
        VLOG_ERR(OVSDB_TXN_COMMIT_ERROR);
    }

    return CMD_OVSDB_FAILURE;
}

DEFUN_NO_FORM (cli_intf_mtu,
        cli_intf_mtu_cmd,
        "mtu",
        "Configure mtu for the interface\n");


/*
 * CLI "duplex"
 * default : full
 */
DEFUN (cli_intf_duplex,
        cli_intf_duplex_cmd,
        "duplex (half|full)",
        "Configure the interface duplex mode\n"
        "Configure half-duplex\n"
        "Configure full-duplex (Default)")
{
    const struct ovsrec_interface * row = NULL;
    struct ovsdb_idl_txn* status_txn = cli_do_config_start();
    enum ovsdb_idl_txn_status status;
    struct smap smap_user_config;

    if (status_txn == NULL)
    {
        VLOG_ERR(OVSDB_TXN_CREATE_ERROR);
        cli_do_config_abort(status_txn);
        return CMD_OVSDB_FAILURE;
    }

    row = ovsrec_interface_first(idl);
    if (!row)
    {
        cli_do_config_abort(status_txn);
        return CMD_OVSDB_FAILURE;
    }

    OVSREC_INTERFACE_FOR_EACH(row, idl)
    {
        if (strcmp(row->name, (char*)vty->index) == 0)
        {
            smap_clone(&smap_user_config, &row->user_config);

            if ((vty_flags & CMD_FLAG_NO_CMD)
                    || (strcmp(argv[0], "full") == 0))
            {
                smap_remove(&smap_user_config,
                        INTERFACE_USER_CONFIG_MAP_DUPLEX);
            }
            else
            {
                smap_replace(&smap_user_config,
                        INTERFACE_USER_CONFIG_MAP_DUPLEX,
                        INTERFACE_USER_CONFIG_MAP_DUPLEX_HALF);
            }
            ovsrec_interface_set_user_config(row, &smap_user_config);
            break;
        }
    }

    status = cli_do_config_finish(status_txn);

    smap_destroy(&smap_user_config);

    if (status == TXN_SUCCESS || status == TXN_UNCHANGED)
    {
        return CMD_SUCCESS;
    }
    else
    {
        VLOG_ERR(OVSDB_TXN_COMMIT_ERROR);
    }

    return CMD_OVSDB_FAILURE;
}

DEFUN_NO_FORM (cli_intf_duplex,
        cli_intf_duplex_cmd,
        "duplex",
        "Configure the interface duplex mode\n");


/*
 * CLI "flowcontrol"
 * default : off
 */
DEFUN (cli_intf_flowcontrol,
        cli_intf_flowcontrol_cmd,
        "flowcontrol (receive|send) (off|on)",
        "Configure interface flow control\n"
        "Receive pause frames\nSend pause frames\n"
        "Turn off flow-control (Default)\nTurn on flow-control\n")
{
    const struct ovsrec_interface * row = NULL;
    struct ovsdb_idl_txn* status_txn = cli_do_config_start();
    enum ovsdb_idl_txn_status status;
    struct smap smap_user_config;

    if (status_txn == NULL)
    {
        VLOG_ERR(OVSDB_TXN_CREATE_ERROR);
        cli_do_config_abort(status_txn);
        return CMD_OVSDB_FAILURE;
    }

    row = ovsrec_interface_first(idl);
    if (!row)
    {
        cli_do_config_abort(status_txn);
        return CMD_OVSDB_FAILURE;
    }

    OVSREC_INTERFACE_FOR_EACH(row, idl)
    {
        if (strcmp(row->name, (char*)vty->index) == 0)
        {
            const char *state_value = smap_get(&row->user_config,
                    INTERFACE_USER_CONFIG_MAP_PAUSE);
            char new_value[INTF_NAME_SIZE] = {0};
            smap_clone(&smap_user_config, &row->user_config);
            if (strcmp(argv[0], "send") == 0)
            {
                if (strcmp(argv[1], "on") == 0)
                {
                    if ((NULL == state_value)
                            || (strcmp(state_value,
                                    INTERFACE_USER_CONFIG_MAP_PAUSE_TX) == 0))
                    {
                        strcpy(new_value, INTERFACE_USER_CONFIG_MAP_PAUSE_TX);
                    }
                    else
                    {
                        strcpy(new_value,
                                INTERFACE_USER_CONFIG_MAP_PAUSE_RXTX);
                    }
                }
                else /* both "flowcontrol send off" and "no flowcontrol send"*/
                {
                    if ((NULL == state_value) ||
                            (strcmp(state_value,
                                    INTERFACE_USER_CONFIG_MAP_PAUSE_TX) == 0))
                    {
                        strcpy(new_value,
                                INTERFACE_USER_CONFIG_MAP_PAUSE_NONE);
                    }
                    else
                    {
                        strcpy(new_value, INTERFACE_USER_CONFIG_MAP_PAUSE_RX);
                    }
                }
            }
            else /* flowcontrol receive */
            {
                if (strcmp(argv[1], "on") == 0)
                {

                    if ((NULL == state_value)
                            || (strcmp(state_value,
                                    INTERFACE_USER_CONFIG_MAP_PAUSE_RX) == 0))
                    {
                        strcpy(new_value, INTERFACE_USER_CONFIG_MAP_PAUSE_RX);
                    }
                    else
                    {
                        strcpy(new_value,
                                INTERFACE_USER_CONFIG_MAP_PAUSE_RXTX);
                    }
                }
                else
                {
                    /* both "flowcontrol receive off"
                       and "no flowcontrol receive" */
                    if ((NULL == state_value) ||
                            (strcmp(state_value,
                                    INTERFACE_USER_CONFIG_MAP_PAUSE_RX) == 0))
                    {
                        strcpy(new_value,
                                INTERFACE_USER_CONFIG_MAP_PAUSE_NONE);
                    }
                    else
                    {
                        strcpy(new_value, INTERFACE_USER_CONFIG_MAP_PAUSE_TX);
                    }
                }
            }

            if (strcmp(new_value, INTERFACE_USER_CONFIG_MAP_PAUSE_NONE) == 0)
            {
                smap_remove(&smap_user_config,
                        INTERFACE_USER_CONFIG_MAP_PAUSE);
            }
            else
            {
                smap_replace(&smap_user_config,
                        INTERFACE_USER_CONFIG_MAP_PAUSE, new_value);
            }
            ovsrec_interface_set_user_config(row, &smap_user_config);
            break;
        }
    }

    status = cli_do_config_finish(status_txn);

    smap_destroy(&smap_user_config);

    if (status == TXN_SUCCESS || status == TXN_UNCHANGED)
    {
        return CMD_SUCCESS;
    }
    else
    {
        VLOG_ERR(OVSDB_TXN_COMMIT_ERROR);
    }

    return CMD_OVSDB_FAILURE;
}

DEFUN_NO_FORM (cli_intf_flowcontrol,
        cli_intf_flowcontrol_cmd,
        "flowcontrol (receive|send)",
        "Configure interface flow control\n"
        "Receive pause frames\nSend pause frames\n");



/*
 * CLI "autonegotiation"
 * default : default
 */
DEFUN (cli_intf_autoneg,
        cli_intf_autoneg_cmd,
        "autonegotiation (off|on)",
        "Configure auto-negotiation process for the interface\n"
        "Turn off autonegotiation (Default)\nTurn on autonegotiation\n")
{
    const struct ovsrec_interface * row = NULL;
    struct ovsdb_idl_txn *status_txn = cli_do_config_start();
    enum ovsdb_idl_txn_status status;
    struct smap smap_user_config;

    if (status_txn == NULL)
    {
        VLOG_ERR(OVSDB_TXN_CREATE_ERROR);
        cli_do_config_abort(status_txn);
        return CMD_OVSDB_FAILURE;
    }

    row = ovsrec_interface_first(idl);
    if (!row)
    {
        cli_do_config_abort(status_txn);
        return CMD_OVSDB_FAILURE;
    }

    OVSREC_INTERFACE_FOR_EACH(row, idl)
    {
        if (strcmp(row->name, (char*)vty->index) == 0)
        {
            smap_clone(&smap_user_config, &row->user_config);

            if (vty_flags & CMD_FLAG_NO_CMD)
            {
                smap_remove(&smap_user_config,
                        INTERFACE_USER_CONFIG_MAP_AUTONEG);
            }
            else
            {
                if (strcmp(INTERFACE_USER_CONFIG_MAP_AUTONEG_DEFAULT,
                        argv[0]) == 0)
                {
                    smap_remove(&smap_user_config,
                            INTERFACE_USER_CONFIG_MAP_AUTONEG);
                }
                else
                {
                    smap_replace(&smap_user_config,
                            INTERFACE_USER_CONFIG_MAP_AUTONEG, argv[0]);
                }
            }
            ovsrec_interface_set_user_config(row, &smap_user_config);
            break;
        }
    }

    status = cli_do_config_finish(status_txn);

    smap_destroy(&smap_user_config);

    if (status == TXN_SUCCESS || status == TXN_UNCHANGED)
    {
        return CMD_SUCCESS;
    }
    else
    {
        VLOG_ERR(OVSDB_TXN_COMMIT_ERROR);
    }

    return CMD_OVSDB_FAILURE;
}

DEFUN_NO_FORM (cli_intf_autoneg,
        cli_intf_autoneg_cmd,
        "autonegotiation",
        "Configure autonegotiation process (Default: off)\n");

/*
 *  Function : port_find
 *  Responsibility : Lookup port table entry for interface name
 *  Parameters :
 *    const char *if_name : Interface name
 *  Return : bool : returns true/false
*/
const struct ovsrec_port*
port_find(const char *if_name)
{
    const struct ovsrec_port *port_row = NULL;
    OVSREC_PORT_FOR_EACH(port_row, idl)
    {
        if (strcmp(port_row->name, if_name) == 0) {
            return port_row;
        }
    }
    return NULL;
}

/*
 * Function : port_match_in_vrf
 * Responsibility : Lookup VRF to which interface is connected
 * Parameters :
 *   const struct ovsrec_port *port_row: pointer to port_row
 *                                       for looking up VRF
 * Return : pointer to VRF row
 */
const struct ovsrec_vrf*
port_match_in_vrf (const struct ovsrec_port *port_row)
{
  const struct ovsrec_vrf *vrf_row = NULL;
  size_t i;
  OVSREC_VRF_FOR_EACH(vrf_row, idl)
    {
      for (i = 0; i < vrf_row->n_ports; i++)
        {
          if (vrf_row->ports[i] == port_row)
            return vrf_row;
        }
    }
  return NULL;
}

/*
 * Function : remove_port_reference
 * Responsibility : Remove port reference from VRF / bridge
 *
 * Parameters :
 *   const struct ovsrec_port *port_row: port to be deleted
 */
static void
remove_port_reference (const struct ovsrec_port *port_row)
{
  struct ovsrec_port **ports;
  const struct ovsrec_vrf *vrf_row = NULL;
  const struct ovsrec_bridge *default_bridge_row = NULL;
  int i,n;

  if (check_port_in_vrf(port_row->name))
    {
      vrf_row = port_match_in_vrf (port_row);
      ports = xmalloc (sizeof *vrf_row->ports * (vrf_row->n_ports - 1));
      for (i = n = 0; i < vrf_row->n_ports; i++)
        {
          if (vrf_row->ports[i] != port_row)
            ports[n++] = vrf_row->ports[i];
        }
      ovsrec_vrf_set_ports (vrf_row, ports, n);
    }

  if (check_port_in_bridge(port_row->name))
    {
      default_bridge_row = ovsrec_bridge_first (idl);
      ports = xmalloc (
          sizeof *default_bridge_row->ports *
          (default_bridge_row->n_ports - 1));
      for (i = n = 0; i < default_bridge_row->n_ports; i++)
        {
          if (default_bridge_row->ports[i] != port_row)
            ports[n++] = default_bridge_row->ports[i];
        }
      ovsrec_bridge_set_ports (default_bridge_row, ports, n);
    }
  free (ports);
}

/*
 * Function : handle_port_config
 * Responsibility : Handle deletion of all configuration
 *                  for split/no split cases
 * Parameters :
 *   const struct ovsrec_port *if_row: pointer to interface row
 *   bool split: Boolean to identify if parent interface is split or not
 */
static void
handle_port_config (const struct ovsrec_interface *if_row, bool split)
{
  const struct ovsrec_port *port_row = NULL;
  int i;
  if (split)
    {
      port_row = port_find (if_row->name);
      if (port_row)
        {
          remove_port_reference (port_row);
          ovsrec_port_delete (port_row);
        }
    }
  else
    {
      for (i = 0; i < if_row->n_split_children; i++)
        {
          port_row = port_find (if_row->split_children[i]->name);
          if (port_row)
            {
              remove_port_reference (port_row);
              ovsrec_port_delete (port_row);
            }
        }
    }
}

/*
 * CLI "split"
 * default : no split
 */
DEFUN (cli_intf_split,
        cli_intf_split_cmd,
        "split",
        "Configure QSFP interface for 4x 10Gb operation "
        "or 1x 40Gb operation.\n")
{
  const struct ovsrec_interface * row = NULL;
  struct ovsdb_idl_txn *status_txn = cli_do_config_start();
  enum ovsdb_idl_txn_status status;
  char flag = '0';
  bool proceed = false;

  if (status_txn == NULL)
    {
      VLOG_ERR (OVSDB_TXN_CREATE_ERROR);
      cli_do_config_abort (status_txn);
      return CMD_OVSDB_FAILURE;
    }

  row = ovsrec_interface_first (idl);
  if (!row)
    {
      cli_do_config_abort (status_txn);
      return CMD_OVSDB_FAILURE;
    }

  OVSREC_INTERFACE_FOR_EACH(row, idl)
    {
      if (strcmp(row->name, (char*)vty->index) == 0)
        {
          /* if not splittable, warn */
          const char *split_value = NULL;
          struct smap smap_user_config;

          smap_clone(&smap_user_config,&row->user_config);

          split_value = smap_get(&row->hw_intf_info,
                  INTERFACE_HW_INTF_INFO_MAP_SPLIT_4);
          if ((split_value == NULL) ||
                  (strcmp(split_value,
                          INTERFACE_HW_INTF_INFO_MAP_SPLIT_4_TRUE) != 0))
            {
              if ((vty_flags & CMD_FLAG_NO_CMD) == 0)
                {
                  vty_out(vty, "Warning: split operation only applies to"
                          " QSFP interfaces with split capability%s",
                          VTY_NEWLINE);
                }
            }
          if (vty_flags & CMD_FLAG_NO_CMD)
            {
              smap_remove(&smap_user_config,
                      INTERFACE_USER_CONFIG_MAP_LANE_SPLIT);
            }
          else
            {
              smap_replace(&smap_user_config,
                      INTERFACE_USER_CONFIG_MAP_LANE_SPLIT,
                      INTERFACE_USER_CONFIG_MAP_LANE_SPLIT_SPLIT);
            }
          ovsrec_interface_set_user_config (row, &smap_user_config);
          /* Reconfiguration should only be done when split/no split commands
           * are entered on split parent interfaces */
          if ((split_value != NULL) &&
              (strcmp(split_value,
                      INTERFACE_HW_INTF_INFO_MAP_SPLIT_4_TRUE) == 0))
            {
              if (vty_flags & CMD_FLAG_NO_CMD)
                {
                  vty_out (vty, "Warning: This will remove all L2/L3 configuration"
                           " on child interfaces.\nDo you want to continue [y/n]? ");
                }
              else
                {
                  vty_out (vty, "Warning: This will remove all L2/L3 configuration"
                           " on parent interface.\nDo you want to continue [y/n]? ");
                }
              while (1)
                {
                  scanf (" %c", &flag);
                  if (flag == 'y' || flag == 'Y')
                    {
                      handle_port_config (row,
                                          (vty_flags & CMD_FLAG_NO_CMD) ? false : true);
                      proceed = true;
                      break;
                    }
                  else if (flag == 'n' || flag == 'N')
                    {
                      vty_out (vty,"%s",VTY_NEWLINE);
                      proceed = false;
                      break;
                    }
                  else
                    {
                      vty_out (vty, "\r                                  ");
                      vty_out (vty, "\rDo you wish to continue [y/n]? ");
                    }
                }
            }
          smap_destroy(&smap_user_config);
          break;
        }
    }

  if (!proceed)
    {
      cli_do_config_abort (status_txn);
      return CMD_SUCCESS;
    }

  status = cli_do_config_finish (status_txn);

  if (status == TXN_SUCCESS || status == TXN_UNCHANGED)
    {
      return CMD_SUCCESS;
    }
  else
    {
      VLOG_ERR (OVSDB_TXN_COMMIT_ERROR);
    }

  return CMD_OVSDB_FAILURE;
}

DEFUN_NO_FORM (cli_intf_split,
        cli_intf_split_cmd,
        "split",
        "Configure QSFP interface for 4x 10Gb "
        "operation or 1x 40Gb operation\n");

#define PRINT_INT_HEADER_IN_SHOW_RUN if (!bPrinted) \
{ \
bPrinted = true;\
vty_out (vty, "interface %s %s", row->name, VTY_NEWLINE);\
}

/*
 *  Function : parse_vlan
 *  Responsibility : Used for VLAN related config
 *  Parameters :
 *      const char *if_name           : Name of interface
 *      struct vty* vty               : Used for ouput
 */
static int
parse_vlan(const char *if_name, struct vty* vty)
{
    const struct ovsrec_port *port_row;
    int i;

    port_row = port_find(if_name);
    if (port_row == NULL)
    {
        return 0;
    }

    if (port_row->vlan_mode == NULL)
    {
        return 0;
    }
    else if (strcmp(port_row->vlan_mode, OVSREC_PORT_VLAN_MODE_ACCESS) == 0)
    {
        vty_out(vty, "%3s%s%ld%s", "", "vlan access ",
                *port_row->tag, VTY_NEWLINE);
    }
    else if (strcmp(port_row->vlan_mode, OVSREC_PORT_VLAN_MODE_TRUNK) == 0)
    {
        for (i = 0; i < port_row->n_trunks; i++)
        {
            vty_out(vty, "%3s%s%ld%s", "", "vlan trunk allowed ",
                    port_row->trunks[i], VTY_NEWLINE);
        }
    }
    else if (strcmp(port_row->vlan_mode,
            OVSREC_PORT_VLAN_MODE_NATIVE_UNTAGGED) == 0)
    {
        if (port_row->n_tag == 1)
        {
            vty_out(vty, "%3s%s%ld%s", "", "vlan trunk native ",
                    *port_row->tag, VTY_NEWLINE);
        }
        for (i = 0; i < port_row->n_trunks; i++)
        {
            vty_out(vty, "%3s%s%ld%s", "", "vlan trunk allowed ",
                    port_row->trunks[i], VTY_NEWLINE);
        }
    }
    else if (strcmp(port_row->vlan_mode,
            OVSREC_PORT_VLAN_MODE_NATIVE_TAGGED) == 0)
    {
        if (port_row->n_tag == 1)
        {
            vty_out(vty, "%3s%s%ld%s", "", "vlan trunk native ",
                    *port_row->tag, VTY_NEWLINE);
        }
        vty_out(vty, "%3s%s%s", "", "vlan trunk native tag",VTY_NEWLINE);
        for (i = 0; i < port_row->n_trunks; i++)
        {
            vty_out(vty, "%3s%s%ld%s", "", "vlan trunk allowed ",
                    port_row->trunks[i], VTY_NEWLINE);
        }
    }

    return 0;
}

/*
 *  Function : parse_l3config
 *  Responsibility : Used for L3 related config
 *  Parameters :
 *      const char *if_name           : Name of interface
 *      struct vty* vty               : Used for ouput
 */
static int
parse_l3config(const char *if_name, struct vty *vty)
{
    const struct ovsrec_port *port_row;
    const struct ovsrec_vrf *vrf_row;
    size_t i;

    port_row = port_find(if_name);
    if (!port_row) {
        return 0;
    }
    if (check_iface_in_bridge(if_name)) {
        vty_out(vty, "%3s%s%s", "", "no routing", VTY_NEWLINE);
        parse_vlan(if_name, vty);
    }
    if (check_iface_in_vrf(if_name)) {
        vrf_row = port_match_in_vrf(port_row);
        if (display_l3_info(port_row, vrf_row)) {
            if (strcmp(vrf_row->name, DEFAULT_VRF_NAME) != 0) {
                vty_out(vty, "%3s%s%s%s", "", "vrf attach ", vrf_row->name,
                        VTY_NEWLINE);
            }
            if (port_row->ip4_address) {
                vty_out(vty, "%3s%s%s%s", "", "ip address ",
                        port_row->ip4_address, VTY_NEWLINE);
            }
            for (i = 0; i < port_row->n_ip4_address_secondary; i++) {
                vty_out(vty, "%3s%s%s%s%s", "", "ip address ",
                        port_row->ip4_address_secondary[i], " secondary",
                        VTY_NEWLINE);
            }
            if (port_row->ip6_address) {
                vty_out(vty, "%3s%s%s%s", "", "ipv6 address ",
                        port_row->ip6_address, VTY_NEWLINE);
            }
            for (i = 0; i < port_row->n_ip6_address_secondary; i++) {
                vty_out(vty, "%3s%s%s%s%s", "", "ipv6 address ",
                        port_row->ip6_address_secondary[i], " secondary",
                        VTY_NEWLINE);
            }
        }
    }
    return 0;
}

static int
print_interface_lag(const char *if_name, struct vty *vty, bool *bPrinted)
{
    const struct ovsrec_port *port_row = NULL;
    const struct ovsrec_interface *if_row = NULL;
    int k=0;

    OVSREC_PORT_FOR_EACH(port_row, idl)
    {
        if (strncmp(port_row->name, LAG_PORT_NAME_PREFIX, LAG_PORT_NAME_PREFIX_LENGTH) == 0)
        {
            for (k = 0; k < port_row->n_interfaces; k++)
            {
                if_row = port_row->interfaces[k];
                if(strncmp(if_name, if_row->name, MAX_IFNAME_LENGTH) == 0)
                {
                    if (!(*bPrinted))
                    {
                        *bPrinted = true;
                        vty_out (vty, "interface %s %s", if_name, VTY_NEWLINE);
                    }
                    vty_out(vty, "%3s%s %s%s", "", "lag",
                            &port_row->name[LAG_PORT_NAME_PREFIX_LENGTH], VTY_NEWLINE);
                }
            }
        }
    }
    return 0;
}

static int
parse_lacp_othercfg(const struct smap *ifrow_config, const char *if_name,
                                                struct vty *vty, bool *bPrinted)
{
    const char *data = NULL;

    data = smap_get(ifrow_config, INTERFACE_OTHER_CONFIG_MAP_LACP_PORT_ID);

    if (data)
    {
        if (!(*bPrinted))
        {
            *bPrinted = true;
            vty_out (vty, "interface %s%s", if_name, VTY_NEWLINE);
        }

        vty_out (vty, "%3s%s %s%s", "", "lacp port-id", data, VTY_NEWLINE);
    }

    data = smap_get(ifrow_config, INTERFACE_OTHER_CONFIG_MAP_LACP_PORT_PRIORITY);

    if (data)
    {
        if (!(*bPrinted))
        {
            *bPrinted = true;
            vty_out (vty, "interface %s%s", if_name, VTY_NEWLINE);
        }

            vty_out (vty, "%3s%s %s%s", "", "lacp port-priority", data, VTY_NEWLINE);

    }

    return 0;
}

static int
parse_lag(struct vty *vty)
{
    const char *data = NULL;
    const struct ovsrec_port *port_row = NULL;

    OVSREC_PORT_FOR_EACH(port_row, idl)
    {
        if(strncmp(port_row->name, LAG_PORT_NAME_PREFIX, LAG_PORT_NAME_PREFIX_LENGTH) == 0)
        {
            /* Print the LAG port name because lag port is present. */
            vty_out (vty, "interface lag %s%s", &port_row->name[LAG_PORT_NAME_PREFIX_LENGTH], VTY_NEWLINE);

            if (check_port_in_bridge(port_row->name))
            {
                vty_out (vty, "%3s%s%s", "", "no routing", VTY_NEWLINE);
                parse_vlan(port_row->name, vty);
            }

            data = port_row->lacp;
            if(data && strcmp(data, OVSREC_PORT_LACP_OFF) != 0)
            {
                vty_out (vty, "%3slacp mode %s%s"," ",data, VTY_NEWLINE);
            }

            data = smap_get(&port_row->other_config, "bond_mode");

            if(data)
            {
                vty_out (vty, "%3shash %s%s"," ",data, VTY_NEWLINE);
            }

            data = smap_get(&port_row->other_config, "lacp-fallback-ab");

            if(data)
            {
                if(VTYSH_STR_EQ(data, "true"))
                {
                    vty_out (vty, "%3slacp fallback%s"," ", VTY_NEWLINE);
                }
            }

            data = smap_get(&port_row->other_config, "lacp-time");

            if(data)
            {
                vty_out (vty, "%3slacp rate %s%s"," ", data, VTY_NEWLINE);
            }
        }
    }

    return 0;
}

static int
cli_show_run_interface_exec (struct cmd_element *self, struct vty *vty,
        int flags, int argc, const char *argv[])
{
    const struct ovsrec_interface *row = NULL;
    const char *cur_state =NULL;
    bool bPrinted = false;

    OVSREC_INTERFACE_FOR_EACH(row, idl)
    {
        if (0 != argc)
        {
            if ((NULL != argv[0]) && (0 != strcmp(argv[0], row->name)))
            {
                continue;
            }
        }
        bPrinted = false;
        cur_state = smap_get(&row->user_config,
                INTERFACE_USER_CONFIG_MAP_ADMIN);
        if ((NULL != cur_state)
                && (strcmp(cur_state,
                        OVSREC_INTERFACE_USER_CONFIG_ADMIN_UP) == 0))
        {
            PRINT_INT_HEADER_IN_SHOW_RUN;
            vty_out(vty, "   no shutdown %s", VTY_NEWLINE);
        }

        cur_state = smap_get(&row->user_config,
                INTERFACE_USER_CONFIG_MAP_SPEEDS);
        if ((NULL != cur_state)
                && (strcmp(cur_state,
                        INTERFACE_USER_CONFIG_MAP_SPEEDS_DEFAULT) != 0))
        {
            PRINT_INT_HEADER_IN_SHOW_RUN;
            vty_out(vty, "   speed %s %s", cur_state, VTY_NEWLINE);
        }

        cur_state = smap_get(&row->user_config,
                INTERFACE_USER_CONFIG_MAP_MTU);
        if ((NULL != cur_state)
                && (strcmp(cur_state,
                        INTERFACE_USER_CONFIG_MAP_MTU_DEFAULT) != 0))
        {
            PRINT_INT_HEADER_IN_SHOW_RUN;
            vty_out(vty, "   mtu %s %s", cur_state, VTY_NEWLINE);
        }

        cur_state = smap_get(&row->user_config,
                INTERFACE_USER_CONFIG_MAP_DUPLEX);
        if ((NULL != cur_state)
                && (strcmp(cur_state,
                        INTERFACE_USER_CONFIG_MAP_DUPLEX_FULL) != 0))
        {
            PRINT_INT_HEADER_IN_SHOW_RUN;
            vty_out(vty, "   duplex %s %s", cur_state, VTY_NEWLINE);
        }

        cur_state = smap_get(&row->user_config,
                INTERFACE_USER_CONFIG_MAP_PAUSE);
        if ((NULL != cur_state)
                && (strcmp(cur_state,
                        INTERFACE_USER_CONFIG_MAP_PAUSE_NONE) != 0))
        {
            PRINT_INT_HEADER_IN_SHOW_RUN;
            if (strcmp(cur_state, INTERFACE_USER_CONFIG_MAP_PAUSE_RX) == 0)
            {
                vty_out(vty, "   flowcontrol receive on %s", VTY_NEWLINE);
            }
            else if (strcmp(cur_state,
                    INTERFACE_USER_CONFIG_MAP_PAUSE_TX) == 0)
            {
                vty_out(vty, "   flowcontrol send on %s", VTY_NEWLINE);
            }
            else
            {
                vty_out(vty, "   flowcontrol receive on %s", VTY_NEWLINE);
                vty_out(vty, "   flowcontrol send on %s", VTY_NEWLINE);
            }

            if (0 != argc)  /* filter applied, break - as data printed */
            {
                break;
            }
        }

        cur_state = smap_get(&row->user_config,
                INTERFACE_USER_CONFIG_MAP_AUTONEG);
        if ((NULL != cur_state)
                && (strcmp(cur_state,
                        INTERFACE_USER_CONFIG_MAP_AUTONEG_DEFAULT) != 0))
        {
            PRINT_INT_HEADER_IN_SHOW_RUN;
            vty_out(vty, "   autonegotiation %s %s", cur_state, VTY_NEWLINE);
        }

        cur_state = smap_get(&row->user_config,
                INTERFACE_USER_CONFIG_MAP_LANE_SPLIT);
        if ((NULL != cur_state)
                && (strcmp(cur_state,
                        INTERFACE_USER_CONFIG_MAP_LANE_SPLIT_SPLIT) == 0))
        {
            PRINT_INT_HEADER_IN_SHOW_RUN;
            vty_out(vty, "   split %s", VTY_NEWLINE);
        }

        if (port_find(row->name))
        {
            PRINT_INT_HEADER_IN_SHOW_RUN;
        }

        parse_l3config(row->name, vty);

        parse_lacp_othercfg(&row->other_config, row->name, vty, &bPrinted);

        print_interface_lag(row->name, vty, &bPrinted);

        if (bPrinted)
        {
            vty_out(vty, "   exit%s", VTY_NEWLINE);
        }
    }

    parse_lag(vty);

    return CMD_SUCCESS;
}

DEFUN (cli_intf_show_run_intf,
        cli_intf_show_run_intf_cmd,
        "show running-config interface",
        SHOW_STR
        "Current running configuration\n"
        INTERFACE_STR)
{
    return cli_show_run_interface_exec (self, vty, vty_flags, 0, 0);
}

DEFUN (cli_intf_show_run_intf_if,
        cli_intf_show_run_intf_if_cmd,
        "show running-config interface IFNAME",
        SHOW_STR
        "Current running configuration\n"
        INTERFACE_STR
        IFNAME_STR)
{
    return cli_show_run_interface_exec (self, vty, vty_flags, 1, argv);
}

int
cli_show_run_interface_mgmt_exec (struct cmd_element *self, struct vty *vty)
{
    const struct ovsrec_system *vswrow;
    const char *data = NULL;
    const char *ip = NULL;
    const char *subnet = NULL;
    const char *dns_1 = NULL;
    const char *dns_2 = NULL;

    vswrow = ovsrec_system_first(idl);
    if (!vswrow)
    {
        VLOG_ERR(OVSDB_ROW_FETCH_ERROR);
        return CMD_OVSDB_FAILURE;
    }

    data = smap_get(&vswrow->mgmt_intf,SYSTEM_MGMT_INTF_MAP_MODE);
    if (!data)
    {
        /* If not present then mode is dhcp.
           So nothing to display since dhcp is the default. */
        return e_vtysh_ok;
    }

    if (VTYSH_STR_EQ(data, SYSTEM_MGMT_INTF_MAP_MODE_STATIC))
    {
        vty_out(vty, "%s%s", "interface mgmt", VTY_NEWLINE);
        ip = smap_get(&vswrow->mgmt_intf,SYSTEM_MGMT_INTF_MAP_IP);
        subnet = smap_get(&vswrow->mgmt_intf,SYSTEM_MGMT_INTF_MAP_SUBNET_MASK);
        if (ip && subnet && (strcmp(ip,MGMT_INTF_DEFAULT_IP) != 0) )
            vty_out(vty, "%4sip static %s/%s%s","",ip,subnet, VTY_NEWLINE);

        ip = smap_get(&vswrow->mgmt_intf,SYSTEM_MGMT_INTF_MAP_IPV6);
        if (ip && (strcmp(ip,MGMT_INTF_DEFAULT_IPV6) != 0))
        {
            vty_out(vty, "%4sip static %s %s%s","",ip,subnet, VTY_NEWLINE);
        }
    }
    else
        return CMD_SUCCESS;

    data = smap_get(&vswrow->mgmt_intf,SYSTEM_MGMT_INTF_MAP_DEFAULT_GATEWAY);
    if (data && (strcmp(data,MGMT_INTF_DEFAULT_IP) != 0))
    {
        vty_out(vty, "%4sdefault-gateway %s%s","",data, VTY_NEWLINE);
    }

    /* Ipv6 show running commands */

    data = smap_get(&vswrow->mgmt_intf,
            SYSTEM_MGMT_INTF_MAP_DEFAULT_GATEWAY_V6);
    if (data && (strcmp(data,MGMT_INTF_DEFAULT_IPV6) != 0))
    {
        vty_out(vty, "%4sdefault-gateway %s%s","",data, VTY_NEWLINE);
    }

    dns_1 = smap_get(&vswrow->mgmt_intf,SYSTEM_MGMT_INTF_MAP_DNS_SERVER_1);
    dns_2 = smap_get(&vswrow->mgmt_intf,SYSTEM_MGMT_INTF_MAP_DNS_SERVER_2);
    if (dns_1 && dns_2 && (strcmp(dns_1,MGMT_INTF_DEFAULT_IP) != 0) &&
            (strcmp(dns_2,MGMT_INTF_DEFAULT_IP) != 0))
    {
        vty_out(vty, "%4snameserver %s %s%s","", dns_1,dns_2, VTY_NEWLINE);
    }else if (dns_1 && (strcmp(dns_1,MGMT_INTF_DEFAULT_IP) != 0))
    {
        vty_out(vty, "%4snameserver %s %s","", dns_1, VTY_NEWLINE);
    }

    return CMD_SUCCESS;

}

DEFUN (cli_intf_show_run_intf_mgmt,
        cli_intf_show_run_intf_mgmt_cmd,
        "show running-config interface mgmt",
        SHOW_STR
        "Current running configuration\n"
        INTERFACE_STR
        "management interface\n")
{
    return cli_show_run_interface_mgmt_exec (self, vty);
}

int cli_show_xvr_exec (struct cmd_element *self, struct vty *vty,
        int flags, int argc, const char *argv[], bool brief)
{
    const struct ovsrec_interface *ifrow = NULL;
    const char *cur_state =NULL;
    struct shash sorted_interfaces;
    const struct shash_node **nodes;
    int idx, count;

    struct string_pairs
    {
        char *key;
        char *string;
    };

    static struct string_pairs pluggable_keys [] = {
        { "connector_status", "Connector status" },
        { "vendor_name", "Vendor name" },
        { "vendor_part_number", "Part number" },
        { "vendor_revision", "Part revision" },
        { "vendor_serial_number", "Serial number" },
        { "supported_speeds", "Supported speeds" },
        { NULL, NULL }
    };

    if (brief)
    {
        vty_out(vty, "%s", VTY_NEWLINE);
        vty_out(vty, "-----------------------------------------------%s",
                VTY_NEWLINE);
        vty_out(vty, "Ethernet      Connector    Module     Module   %s",
                VTY_NEWLINE);
        vty_out(vty, "Interface                  Type       Status   %s",
                VTY_NEWLINE);
        vty_out(vty, "-----------------------------------------------%s",
                VTY_NEWLINE);
    }
    else
    {
        vty_out (vty, "%s", VTY_NEWLINE);
    }

    shash_init(&sorted_interfaces);

    OVSREC_INTERFACE_FOR_EACH(ifrow, idl)
    {
        shash_add(&sorted_interfaces, ifrow->name, (void *)ifrow);
    }

    nodes = sort_interface(&sorted_interfaces);

    count = shash_count(&sorted_interfaces);

    for (idx = 0; idx < count; idx++)
    {
        ifrow = (const struct ovsrec_interface *)nodes[idx]->data;

        if ((NULL != argv[0]) && (0 != strcmp(argv[0],ifrow->name)))
        {
            continue;
        }

        if (strcmp(ifrow->type, OVSREC_INTERFACE_TYPE_INTERNAL) == 0)
        {
            /* Skipping internal interfaces */
            continue;
        }

        if (brief)
        {
            /* Display the brief information */
            vty_out (vty, " %-12s ", ifrow->name);
            bool present = false;
            cur_state = smap_get(&ifrow->hw_intf_info,
                    INTERFACE_HW_INTF_INFO_MAP_CONNECTOR);
            if (cur_state != NULL)
            {
                if (strcmp(cur_state,
                        INTERFACE_HW_INTF_INFO_MAP_CONNECTOR_RJ45) ==0)
                {
                    vty_out(vty, "  RJ45       ");
                }
                else if (strcmp(cur_state,
                        INTERFACE_HW_INTF_INFO_MAP_CONNECTOR_SFP_PLUS) ==0)
                {
                    vty_out(vty, "  SFP+       ");
                }
                else if (strcmp(cur_state,
                        INTERFACE_HW_INTF_INFO_MAP_CONNECTOR_QSFP_PLUS) ==0)
                {
                    vty_out(vty, "  QSFP       ");
                }
                else
                {
                    vty_out(vty, "             ");
                }
            }

            cur_state = smap_get(&ifrow->hw_intf_info,
                    INTERFACE_HW_INTF_INFO_MAP_PLUGGABLE);

            if (cur_state != NULL && strcmp(cur_state,
                    INTERFACE_HW_INTF_INFO_MAP_PLUGGABLE_TRUE) == 0)
            {
                cur_state = smap_get(&ifrow->pm_info,
                        INTERFACE_PM_INFO_MAP_CONNECTOR);
                if (cur_state != NULL)
                {
                    if (strcmp(cur_state,
                            OVSREC_INTERFACE_PM_INFO_CONNECTOR_ABSENT) == 0)
                    {
                        vty_out(vty, "%-11s", "--");
                        present = false;
                    }
                    else
                    {
                        vty_out(vty, "%-11s", cur_state);
                        present = true;
                    }
                }
                else
                {
                    vty_out(vty, "%-11s", "--");
                }

                cur_state = smap_get(&ifrow->pm_info,
                        INTERFACE_PM_INFO_MAP_CONNECTOR_STATUS);
                if (present && cur_state != NULL)
                {
                    vty_out(vty, "%-11s", cur_state);
                }
                else
                {
                    vty_out(vty, "%-11s", "--");
                }
            }
            else
            {
                vty_out(vty, "%-11s", "--");
                vty_out(vty, "%-11s", "--");
            }
            vty_out(vty, "%s", VTY_NEWLINE);
        }
        else
        {
            int i;

            /* Display transceiver information */
            vty_out (vty, "Interface %s:%s", ifrow->name, VTY_NEWLINE);

            cur_state = smap_get(&ifrow->hw_intf_info,
                    INTERFACE_HW_INTF_INFO_MAP_CONNECTOR);
            if (NULL != cur_state)
            {
                if (strcmp(cur_state,
                        INTERFACE_HW_INTF_INFO_MAP_CONNECTOR_RJ45) == 0)
                {
                    vty_out(vty, " Connector: RJ45%s", VTY_NEWLINE);
                }
                else if (strcmp(cur_state,
                        INTERFACE_HW_INTF_INFO_MAP_CONNECTOR_SFP_PLUS) == 0)
                {
                    vty_out(vty, " Connector: SFP+%s", VTY_NEWLINE);
                }
                else if (strcmp(cur_state,
                        INTERFACE_HW_INTF_INFO_MAP_CONNECTOR_QSFP_PLUS) == 0)
                {
                    cur_state = smap_get(&ifrow->hw_intf_info,
                            INTERFACE_HW_INTF_INFO_MAP_SPLIT_4);
                    if (cur_state != NULL &&
                            strcmp(cur_state,
                               INTERFACE_HW_INTF_INFO_MAP_SPLIT_4_TRUE) == 0)
                    {
                        vty_out(vty, " Connector: QSFP (splittable)%s",
                                VTY_NEWLINE);
                    }
                    else
                    {
                        vty_out(vty, " Connector: QSFP %s", VTY_NEWLINE);
                    }
                }
                else
                {
                    vty_out(vty, " Connector: %s%s", cur_state, VTY_NEWLINE);
                }
            }

            cur_state = smap_get(&ifrow->hw_intf_info,
                    INTERFACE_HW_INTF_INFO_MAP_PLUGGABLE);

            if (cur_state != NULL && strcmp(cur_state,
                    INTERFACE_HW_INTF_INFO_MAP_PLUGGABLE_TRUE) == 0)
            {
                cur_state = smap_get(&ifrow->pm_info,
                        INTERFACE_PM_INFO_MAP_CONNECTOR);
                if (cur_state != NULL)
                {
                    if (strcmp(cur_state,
                            OVSREC_INTERFACE_PM_INFO_CONNECTOR_ABSENT) == 0)
                    {
                        vty_out(vty, " Transceiver module: not present%s",
                                VTY_NEWLINE);
                    }
                    else
                    {
                        vty_out(vty, " Transceiver module: %s%s", cur_state,
                                VTY_NEWLINE);
                        for (i = 0; pluggable_keys[i].key != NULL; i++)
                        {
                            vty_out(vty, " %s: ", pluggable_keys[i].string);
                            cur_state = smap_get(&ifrow->pm_info,
                                    pluggable_keys[i].key);
                            if (cur_state != NULL)
                            {
                                vty_out(vty, "%s%s", cur_state, VTY_NEWLINE);
                            }
                            else
                            {
                                vty_out(vty, "%s", VTY_NEWLINE);
                            }
                        }
                    }
                }
                else
                {
                    vty_out(vty,
                            " Transceiver module: no information available%s",
                            VTY_NEWLINE);
                }
            }
            vty_out (vty, "%s", VTY_NEWLINE);
        }
    }

    shash_destroy(&sorted_interfaces);
    free(nodes);

    return CMD_SUCCESS;
}


/*-----------------------------------------------------------------------------
  | Function : show_ip_addresses
  | Responsibility : Used to show ip addresses for L3 interfaces
  | Parameters :
  |     const char *if_name           : Name of interface
  |     struct vty* vty               : Used for ouput
  -----------------------------------------------------------------------------*/
static int
show_ip_addresses(const char *if_name, struct vty *vty)
{
    const struct ovsrec_port *port_row;
    const struct ovsrec_vrf *vrf_row;
    size_t i;

    port_row = port_find(if_name);
    if (!port_row) {
        return 0;
    }

    if (check_iface_in_vrf(if_name)) {
        vrf_row = port_match_in_vrf(port_row);
        if (display_l3_info(port_row, vrf_row)) {
            if (port_row->ip4_address) {
                vty_out(vty, " IPv4 address %s%s", port_row->ip4_address,
                        VTY_NEWLINE);
            }
            for (i = 0; i < port_row->n_ip4_address_secondary; i++) {
                vty_out(vty, " IPv4 address %s secondary%s",
                        port_row->ip4_address_secondary[i],
                        VTY_NEWLINE);
            }
            if (port_row->ip6_address) {
                vty_out(vty, " IPv6 address %s%s", port_row->ip6_address,
                        VTY_NEWLINE);
            }
            for (i = 0; i < port_row->n_ip6_address_secondary; i++) {
                vty_out(vty, " IPv6 address %s secondary%s",
                        port_row->ip6_address_secondary[i],
                        VTY_NEWLINE);
            }
        }
    }
    return 0;
}


void
show_lacp_interfaces_brief (struct vty *vty, const char *argv[])
{
    const struct ovsrec_port *lag_port = NULL;
    const struct ovsrec_interface *if_row = NULL;
    const char *aggregate_mode = NULL;
    const struct ovsdb_datum *datum;

    int64_t lag_speed = 0;

    // Index for loops
    int interface_index = 0;

    OVSREC_PORT_FOR_EACH(lag_port, idl)
    {
        if ((NULL != argv[0]) && (0 != strcmp(argv[0],lag_port->name)))
        {
            continue;
        }

        if(strncmp(lag_port->name, LAG_PORT_NAME_PREFIX, LAG_PORT_NAME_PREFIX_LENGTH) != 0)
        {
            continue;
        }

        vty_out(vty, " %-12s ", lag_port->name);
        vty_out(vty, "--      "); /*VLAN */
        vty_out(vty, "--  "); /*type */

        aggregate_mode = lag_port->lacp;
        if(aggregate_mode)
            vty_out(vty, "%-7s ", aggregate_mode); /* mode -  active, passive*/
        else
            vty_out(vty, "--      "); /* mode -  active, passive */

        vty_out(vty, "--     ");/* Status */
        vty_out(vty, "--                       "); /*Reason*/

        /* Speed calculation: Adding speed of all aggregated interfaces*/
        lag_speed = 0;
        for (interface_index = 0; interface_index < lag_port->n_interfaces; interface_index++)
        {
            if_row = lag_port->interfaces[interface_index];

            datum = ovsrec_interface_get_link_speed(if_row, OVSDB_TYPE_INTEGER);
            if ((NULL!=datum) && (datum->n >0))
            {
                lag_speed += datum->keys[0].integer;
            }
        }
        if(lag_speed == 0)
        {
            vty_out(vty, " %-6s", "auto");
        }
        else
        {
            vty_out(vty, " %-6ld", lag_speed/1000000);
        }

        vty_out(vty, "   -- ");  /* Port channel */
        vty_out(vty, "%s", VTY_NEWLINE);
    }
}

void
show_lacp_interfaces (struct vty *vty, char* interface_statistics_keys[],
                      const char *argv[])
{
    const struct ovsrec_port *lag_port = NULL;
    const struct ovsrec_interface *if_row = NULL;
    const char *aggregate_mode = NULL;
    const struct ovsdb_datum *datum;
    unsigned int index;

    int64_t lag_speed = 0;

    // Indexes for loops
    int interface_index = 0;
    int stat_index = 0;

    // Array to keep the statistics for each lag while adding the
    // stats for each interface in the lag.
    int lag_statistics [12] = {0};

    OVSREC_PORT_FOR_EACH(lag_port, idl)
    {
        union ovsdb_atom atom;

        if ((NULL != argv[0]) && (0 != strcmp(argv[0],lag_port->name)))
        {
            continue;
        }

        if(strncmp(lag_port->name, LAG_PORT_NAME_PREFIX, LAG_PORT_NAME_PREFIX_LENGTH) != 0)
        {
            continue;
        }

        vty_out(vty, "Aggregate-name %s %s", lag_port->name, VTY_NEWLINE);
        vty_out(vty, " Aggregated-interfaces : ");


        lag_speed = 0;
        for (interface_index = 0; interface_index < lag_port->n_interfaces; interface_index++)
        {
            if_row = lag_port->interfaces[interface_index];
            vty_out(vty, "%s ", if_row->name);

            datum = ovsrec_interface_get_link_speed(if_row, OVSDB_TYPE_INTEGER);
            if ((NULL!=datum) && (datum->n >0))
            {
                lag_speed += datum->keys[0].integer;
            }

            datum = ovsrec_interface_get_statistics(if_row,
                                OVSDB_TYPE_STRING, OVSDB_TYPE_INTEGER);

            // Adding statistic value for each interface in the lag
            for (stat_index = 0; stat_index < sizeof(lag_statistics)/sizeof(int); stat_index++)
            {
                atom.string = interface_statistics_keys[stat_index];
                index = ovsdb_datum_find_key(datum, &atom, OVSDB_TYPE_STRING);
                lag_statistics[stat_index] += (index == UINT_MAX)? 0 : datum->values[index].integer;
            }
        }
        vty_out(vty, "%s", VTY_NEWLINE);
        aggregate_mode = lag_port->lacp;
        if(aggregate_mode)
            vty_out(vty, " Aggregate mode : %s %s", aggregate_mode, VTY_NEWLINE);
        vty_out(vty, " Speed %ld Mb/s %s",lag_speed/1000000 , VTY_NEWLINE);
        vty_out(vty, " RX%s", VTY_NEWLINE);
        vty_out(vty, "   %10ld input packets  ", lag_statistics[0]);
        vty_out(vty, "   %10ld bytes  ",lag_statistics[1]);
        vty_out(vty, "%s", VTY_NEWLINE);

        vty_out(vty, "   %10ld input error    ",lag_statistics[8]);
        vty_out(vty, "   %10ld dropped  ",lag_statistics[4]);
        vty_out(vty, "%s", VTY_NEWLINE);

        vty_out(vty, "   %10ld CRC/FCS  ",lag_statistics[7]);
        vty_out(vty, "%s", VTY_NEWLINE);
        vty_out(vty, " TX%s", VTY_NEWLINE);

        vty_out(vty, "   %10ld output packets ",lag_statistics[2]);
        vty_out(vty, "   %10ld bytes  ",lag_statistics[3]);
        vty_out(vty, "%s", VTY_NEWLINE);

        vty_out(vty, "   %10ld input error    ",lag_statistics[11]);
        vty_out(vty, "   %10ld dropped  ",lag_statistics[9]);
        vty_out(vty, "%s", VTY_NEWLINE);

        vty_out(vty, "   %10ld collision  ",lag_statistics[10]);
        vty_out(vty, "%s", VTY_NEWLINE);
        vty_out(vty, "%s", VTY_NEWLINE);
    }
}


int
cli_show_interface_exec (struct cmd_element *self, struct vty *vty,
        int flags, int argc, const char *argv[], bool brief)
{
    const struct ovsrec_interface *ifrow = NULL;
    const char *cur_state =NULL;
    struct shash sorted_interfaces;
    const struct shash_node **nodes;
    int idx, count;

    const struct ovsdb_datum *datum;
    static char *interface_statistics_keys [] = {
        "rx_packets",
        "rx_bytes",
        "tx_packets",
        "tx_bytes",
        "rx_dropped",
        "rx_frame_err",
        "rx_over_err",
        "rx_crc_err",
        "rx_errors",
        "tx_dropped",
        "collisions",
        "tx_errors"
    };

    unsigned int index;
    int64_t intVal = 0;

    if (brief)
    {
        /* Display the brief information */
        vty_out(vty, "%s", VTY_NEWLINE);
        vty_out(vty, "--------------------------------------------------------------------------------%s", VTY_NEWLINE);
        vty_out(vty, "Ethernet      VLAN    Type Mode   Status  Reason                   Speed     Port%s", VTY_NEWLINE);
        vty_out(vty, "Interface                                                          (Mb/s)    Ch#%s", VTY_NEWLINE);
        vty_out(vty, "--------------------------------------------------------------------------------%s", VTY_NEWLINE);
    }
    else
    {
        vty_out (vty, "%s", VTY_NEWLINE);
    }

    shash_init(&sorted_interfaces);

    OVSREC_INTERFACE_FOR_EACH(ifrow, idl)
    {
        shash_add(&sorted_interfaces, ifrow->name, (void *)ifrow);
    }

    nodes = sort_interface(&sorted_interfaces);

    count = shash_count(&sorted_interfaces);

    for (idx = 0; idx < count; idx++)
    {
        union ovsdb_atom atom;

        ifrow = (const struct ovsrec_interface *)nodes[idx]->data;

        if ((NULL != argv[0]) && (0 != strcmp(argv[0],ifrow->name)))
        {
            continue;
        }

        if (strcmp(ifrow->type, OVSREC_INTERFACE_TYPE_INTERNAL) == 0)
        {
            /* Skipping internal interfaces */
            continue;
        }

        if (brief)
        {
            /* Display the brief information */
            vty_out (vty, " %-12s ", ifrow->name);
            vty_out(vty, "--      "); /*vVLAN */
            vty_out(vty, "eth  "); /*type */
            vty_out(vty, "--     "); /* mode - routed or not */

            vty_out (vty, "%-6s ", ifrow->link_state);

            if ((NULL != ifrow->admin_state) &&
                    (strcmp(ifrow->admin_state,
                            OVSREC_INTERFACE_USER_CONFIG_ADMIN_DOWN) == 0))
            {
                vty_out (vty, "Administratively down    ");
            }
            else
            {
                vty_out (vty, "                         ");
            }
            intVal = 0;
            datum = ovsrec_interface_get_link_speed(ifrow, OVSDB_TYPE_INTEGER);
            if ((NULL!=datum) && (datum->n >0))
            {
                intVal = datum->keys[0].integer;
            }

            if(intVal == 0)
            {
                vty_out(vty, " %-6s", "auto");
            }
            else
            {
                vty_out(vty, " %-6ld", intVal/1000000);
            }
            vty_out(vty, "   -- ");  /* Port channel */
            vty_out (vty, "%s", VTY_NEWLINE);
        }
        else
        {
            intVal = 0;

            vty_out (vty, "Interface %s is %s ", ifrow->name, ifrow->link_state);
            if ((NULL != ifrow->admin_state)
                    && strcmp(ifrow->admin_state,
                            OVSREC_INTERFACE_USER_CONFIG_ADMIN_DOWN) == 0)
            {
                vty_out (vty, "(Administratively down) %s", VTY_NEWLINE);
                vty_out (vty, " Admin state is down%s",
                        VTY_NEWLINE);
            }
            else
            {
                vty_out (vty, "%s", VTY_NEWLINE);
                vty_out (vty, " Admin state is up%s", VTY_NEWLINE);
            }

            if (ifrow->error != NULL)
            {
                vty_out (vty, " State information: %s%s",
                        ifrow->error, VTY_NEWLINE);
            }

            vty_out (vty, " Hardware: Ethernet, MAC Address: %s %s",
                    ifrow->mac_in_use, VTY_NEWLINE);

            /* Displaying ipv4 and ipv6 primary and secondary addresses*/
            show_ip_addresses(ifrow->name, vty);

            datum = ovsrec_interface_get_mtu(ifrow, OVSDB_TYPE_INTEGER);
            if ((NULL!=datum) && (datum->n >0))
            {
                intVal = datum->keys[0].integer;
            }

            vty_out(vty, " MTU %ld %s", intVal, VTY_NEWLINE);

            if ((NULL != ifrow->duplex) &&
                    (strcmp(ifrow->duplex, "half") == 0))
            {
                vty_out(vty, " Half-duplex %s", VTY_NEWLINE);
            }
            else
            {
                vty_out(vty, " Full-duplex %s", VTY_NEWLINE);
            }

            intVal = 0;
            datum = ovsrec_interface_get_link_speed(ifrow, OVSDB_TYPE_INTEGER);
            if ((NULL!=datum) && (datum->n >0))
            {
                intVal = datum->keys[0].integer;
            }
            vty_out(vty, " Speed %ld Mb/s %s",intVal/1000000 , VTY_NEWLINE);

            cur_state = smap_get(&ifrow->user_config,
                    INTERFACE_USER_CONFIG_MAP_AUTONEG);
            if ((NULL == cur_state) ||
                    strcmp(cur_state, "off") !=0)
            {
                vty_out(vty, " Auto-Negotiation is turned on %s", VTY_NEWLINE);
            }
            else
            {
                vty_out(vty, " Auto-Negotiation is turned off %s",
                        VTY_NEWLINE);
            }

            cur_state = ifrow->pause;
            if (NULL != cur_state)
            {
                if (strcmp(cur_state,
                        INTERFACE_USER_CONFIG_MAP_PAUSE_NONE) == 0)

                {
                    vty_out(vty, " Input flow-control is off, "
                            "output flow-control is off%s",VTY_NEWLINE);
                }
                else if (strcmp(cur_state,
                        INTERFACE_USER_CONFIG_MAP_PAUSE_RX) == 0)
                {
                    vty_out(vty, " Input flow-control is on, "
                            "output flow-control is off%s",VTY_NEWLINE);
                }
                else if (strcmp(cur_state,
                        INTERFACE_USER_CONFIG_MAP_PAUSE_TX) == 0)
                {
                    vty_out(vty, " Input flow-control is off, "
                            "output flow-control is on%s",VTY_NEWLINE);
                }
                else
                {
                    vty_out(vty, " Input flow-control is on, "
                            "output flow-control is on%s",VTY_NEWLINE);
                }
            }
            else
            {
                vty_out(vty, " Input flow-control is off, "
                        "output flow-control is off%s",VTY_NEWLINE);
            }

            datum = ovsrec_interface_get_statistics(ifrow,
                    OVSDB_TYPE_STRING, OVSDB_TYPE_INTEGER);

            if (NULL==datum) continue;

            vty_out(vty, " RX%s", VTY_NEWLINE);

            atom.string = interface_statistics_keys[0];
            index = ovsdb_datum_find_key(datum, &atom, OVSDB_TYPE_STRING);
            vty_out(vty, "   %10ld input packets  ",
                    (index == UINT_MAX)? 0 : datum->values[index].integer);
            atom.string = interface_statistics_keys[1];
            index = ovsdb_datum_find_key(datum, &atom, OVSDB_TYPE_STRING);
            vty_out(vty, "   %10ld bytes  ",
                    (index == UINT_MAX)? 0 : datum->values[index].integer);
            vty_out(vty, "%s", VTY_NEWLINE);

            atom.string = interface_statistics_keys[8];
            index = ovsdb_datum_find_key(datum, &atom, OVSDB_TYPE_STRING);
            vty_out(vty, "   %10ld input error    ",
                    (index == UINT_MAX)? 0 : datum->values[index].integer);
            atom.string = interface_statistics_keys[4];
            index = ovsdb_datum_find_key(datum, &atom, OVSDB_TYPE_STRING);
            vty_out(vty, "   %10ld dropped  ",
                    (index == UINT_MAX)? 0 : datum->values[index].integer);
            vty_out(vty, "%s", VTY_NEWLINE);

            atom.string = interface_statistics_keys[7];
            index = ovsdb_datum_find_key(datum, &atom, OVSDB_TYPE_STRING);
            vty_out(vty, "   %10ld CRC/FCS  ",
                    (index == UINT_MAX)? 0 : datum->values[index].integer);
            vty_out(vty, "%s", VTY_NEWLINE);

            vty_out(vty, " TX%s", VTY_NEWLINE);

            atom.string = interface_statistics_keys[2];
            index = ovsdb_datum_find_key(datum, &atom, OVSDB_TYPE_STRING);
            vty_out(vty, "   %10ld output packets ",
                    (index == UINT_MAX)? 0 : datum->values[index].integer);
            atom.string = interface_statistics_keys[3];
            index = ovsdb_datum_find_key(datum, &atom, OVSDB_TYPE_STRING);
            vty_out(vty, "   %10ld bytes  ",
                    (index == UINT_MAX)? 0 : datum->values[index].integer);
            vty_out(vty, "%s", VTY_NEWLINE);

            atom.string = interface_statistics_keys[11];
            index = ovsdb_datum_find_key(datum, &atom, OVSDB_TYPE_STRING);
            vty_out(vty, "   %10ld input error    ",
                    (index == UINT_MAX)? 0 : datum->values[index].integer);
            atom.string = interface_statistics_keys[9];
            index = ovsdb_datum_find_key(datum, &atom, OVSDB_TYPE_STRING);
            vty_out(vty, "   %10ld dropped  ",
                    (index == UINT_MAX)? 0 : datum->values[index].integer);
            vty_out(vty, "%s", VTY_NEWLINE);

            atom.string = interface_statistics_keys[10];
            index = ovsdb_datum_find_key(datum, &atom, OVSDB_TYPE_STRING);
            vty_out(vty, "   %10ld collision  ",
                    (index == UINT_MAX)? 0 : datum->values[index].integer);
            vty_out(vty, "%s", VTY_NEWLINE);

            vty_out(vty, "%s", VTY_NEWLINE);

            if (NULL != argv[0])
            {
                break;
            }
        }
    }

    shash_destroy(&sorted_interfaces);
    free(nodes);

    if(brief)
    {
        show_lacp_interfaces_brief(vty, argv);
    }
    else
    {
        show_lacp_interfaces(vty, interface_statistics_keys, argv);
    }

    return CMD_SUCCESS;
}


DEFUN (cli_intf_show_intferface_ifname,
        cli_intf_show_intferface_ifname_cmd,
        "show interface IFNAME {brief|transceiver}",
        SHOW_STR
        INTERFACE_STR
        IFNAME_STR
        "Show brief info of interface\n"
        "Show transceiver info for interface\n")
{
    bool brief = false;
    bool transceiver = false;
    int rc = CMD_SUCCESS;

    if ((NULL != argv[1]) && (strcmp(argv[1], "brief") == 0))
    {
        brief = true;
    }
    if ((NULL != argv[2]) && (strcmp(argv[2], "transceiver") == 0))
    {
        transceiver = true;
    }
    if (transceiver)
    {
        rc = cli_show_xvr_exec (self, vty, vty_flags, argc, argv, brief);
    }
    else
    {
        rc = cli_show_interface_exec (self, vty, vty_flags, argc, argv, brief);
    }

    return rc;
}

DEFUN (cli_intf_show_intferface_ifname_br,
        cli_intf_show_intferface_ifname_br_cmd,
        "show interface {brief|transceiver}",
        SHOW_STR
        INTERFACE_STR
        "Show brief info of interfaces\n"
        "Show transceiver info for interfaces\n")
{
    bool brief = false;
    bool transceiver = false;
    int rc = CMD_SUCCESS;

    if ((NULL != argv[0]) && (strcmp(argv[0], "brief") == 0))
    {
        brief = true;
    }
    if ((NULL != argv[1]) && (strcmp(argv[1], "transceiver") == 0))
    {
        transceiver = true;
    }
    argv[0] = NULL;

    if (transceiver)
    {
        rc = cli_show_xvr_exec (self, vty, vty_flags, argc, argv, brief);
    }
    else
    {
        rc = cli_show_interface_exec (self, vty, vty_flags, argc, argv, brief);
    }
    return rc;
}

#ifdef ENABLE_OVSDB
/* Function : check_internal_vlan
 * Description : Checks if interface vlan is being created for
 * an already used internal VLAN.
 * param in : vlanid - to check if it is already in use
 */
int
check_internal_vlan(uint16_t vlanid)
{
    const struct ovsrec_vlan *vlan_row = NULL;

    OVSREC_VLAN_FOR_EACH(vlan_row, idl)
    {
        if (smap_get(&vlan_row->internal_usage,
                VLAN_INTERNAL_USAGE_L3PORT))
        {
            VLOG_DBG("%s Used internally for l3 interface", __func__);
            /* now check if this vlan is used for creating vlan interface */
            if (vlanid == vlan_row->id) {
                VLOG_DBG("%s This is a internal vlan = %d", __func__, vlanid);
                return 0;
            }
        }
    }

    return 1;
}

/* Function : create_vlan_interface
 * Description : Creates a vlan interface. Will create an
 * interface and port with name same as for VLAN interface.
 * and then associate it with the VRF and Bridge row.
 * param in : vlan_if - Vlan interface name
 */
int
create_vlan_interface(const char *vlan_if)
{
    const struct ovsrec_interface *if_row = NULL;
    const struct ovsrec_port *port_row = NULL;
    const struct ovsrec_bridge *bridge_row = NULL;
    const struct ovsrec_vrf *vrf_row = NULL;

    struct ovsdb_idl_txn *status_txn = NULL;
    enum ovsdb_idl_txn_status status;

    struct ovsrec_port **vrf_port_list;
    struct ovsrec_port **bridge_port_list;
    struct ovsrec_interface **iface_list;

    bool intf_exist = false, port_exist = false;

    int i;
    int64_t tag = atoi(vlan_if + 4);

    ovsdb_idl_run(idl);

    status_txn = cli_do_config_start();

    if (!status_txn) {
        VLOG_ERR(
                "%s Got an error when trying to create a transaction"
                " using ovsdb_idl_txn_create()", __func__);
        cli_do_config_abort(status_txn);
        return CMD_OVSDB_FAILURE;
    }

    /*verify if interface exists */
    OVSREC_INTERFACE_FOR_EACH(if_row, idl)
    {
        if (strcmp(if_row->name, vlan_if) == 0) {
            intf_exist = true;
        }
    }

    /*verify if port exists */
    OVSREC_PORT_FOR_EACH(port_row, idl)
    {
        if (strcmp(port_row->name, vlan_if) == 0) {
            port_exist = true;
        }
    }

    /* If both port and interface exists return success
       nothing to change here */
    if (intf_exist == true && port_exist == true) {
        VLOG_DBG("%s Both interface and port exists"
                " for this Vlan interface name", __func__);
        cli_do_config_finish(status_txn);
        return CMD_SUCCESS;
    } else if (!(intf_exist == false && port_exist == false)) {
        /* Only if both do not exist then go ahead else return ERROR */
        VLOG_ERR(
                "%s Interface OR Port row already exists for this Vlan"
                " interface. Ideally we should either have BOTH already"
                " existing or BOTH non existing.", __func__);
        cli_do_config_abort(status_txn);
        return CMD_OVSDB_FAILURE;
    }

    /* Get vrf row so that we can add the port to it */
    OVSREC_VRF_FOR_EACH (vrf_row, idl)
    {
        if (strcmp(vrf_row->name, DEFAULT_VRF_NAME) == 0) {
            break;
        }
    }

    if (!vrf_row) {
        VLOG_ERR("%s Error: Could not fetch VRF data.", __func__);
        VLOG_DBG(
                "%s VRF table did not have any rows. Ideally it"
                " should have just one entry.", __func__);
        cli_do_config_abort(status_txn);
        return CMD_OVSDB_FAILURE;
    }

    /* Get Bridge row so that we can add the port to it */
    OVSREC_BRIDGE_FOR_EACH (bridge_row, idl)
    {
        if (strcmp(bridge_row->name, DEFAULT_BRIDGE_NAME) == 0) {
            break;
        }
    }

    if (!bridge_row) {
        VLOG_ERR("%s Error: Could not fetch Bridge data.", __func__);
        VLOG_DBG(
                "%s Bridge table did not have any rows.", __func__);
        cli_do_config_abort(status_txn);
        return CMD_OVSDB_FAILURE;
    }

    /* adding an interface */
    if_row = ovsrec_interface_insert(status_txn);
    ovsrec_interface_set_name(if_row, vlan_if);
    ovsrec_interface_set_type(if_row, OVSREC_INTERFACE_TYPE_INTERNAL);

    struct smap smap_user_config;
    smap_clone(&smap_user_config,&if_row->user_config);

    /* Set the admin state */
    smap_replace(&smap_user_config, INTERFACE_USER_CONFIG_MAP_ADMIN,
            OVSREC_INTERFACE_USER_CONFIG_ADMIN_UP);

    ovsrec_interface_set_user_config(if_row, &smap_user_config);
    smap_destroy(&smap_user_config);

    iface_list = xmalloc(sizeof(struct ovsrec_interface));
    iface_list[0] = (struct ovsrec_interface *)if_row;

    /* Adding a port to the corresponding interface*/
    port_row = ovsrec_port_insert(status_txn);
    ovsrec_port_set_name(port_row, vlan_if);
    ovsrec_port_set_interfaces(port_row, iface_list, 1);
    ovsrec_port_set_tag(port_row, &tag, 1);
    ovsrec_port_set_vlan_mode(port_row, 0 /*PORT_VLAN_ACCESS*/);

    /* Add the port to vrf port list */
    vrf_port_list =
        xmalloc(sizeof(struct ovsrec_port) * (vrf_row->n_ports + 1));
    for (i = 0; i < vrf_row->n_ports; i++) {
        vrf_port_list[i] = vrf_row->ports[i];
    }
    vrf_port_list[vrf_row->n_ports] = (struct ovsrec_port *)port_row;
    ovsrec_vrf_set_ports(vrf_row, vrf_port_list, vrf_row->n_ports + 1);
    free(vrf_port_list);

    /* Add the port to bridge */
    bridge_port_list =
        xmalloc(sizeof(struct ovsrec_port) * (bridge_row->n_ports + 1));
    for (i = 0; i < bridge_row->n_ports; i++) {
        bridge_port_list[i] = bridge_row->ports[i];
    }
    bridge_port_list[bridge_row->n_ports] = (struct ovsrec_port *)port_row;
    ovsrec_bridge_set_ports(bridge_row,
            bridge_port_list, bridge_row->n_ports + 1);
    free(bridge_port_list);

    status = cli_do_config_finish(status_txn);

    if (status == TXN_SUCCESS) {
        VLOG_DBG(
                "%s The command succeeded and interface \"%s\" was create",
                __func__, vlan_if);
        return CMD_SUCCESS;
    } else if (status == TXN_UNCHANGED) {
        VLOG_DBG(
                "%s The command resulted in no change "
                "Check if interface \"%s\" was create",
                __func__, vlan_if);
        return CMD_SUCCESS;
    } else {
        VLOG_ERR(
                "%s While trying to commit transaction to DB, got a status"
                " response : %s", __func__,
                ovsdb_idl_txn_status_to_string(status));
        return CMD_OVSDB_FAILURE;
    }
}

/* Function : delete_vlan_interface
 * Description : Deletes a vlan interface. Will delete the
 * interface and port with name same as for VLAN interface.
 * and then remove port associated in VRF and Bridge row.
 * param in : vlan_if - Vlan interface name
 */
int
delete_vlan_interface(const char *vlan_if)
{
    const struct ovsrec_interface *if_row = NULL;
    const struct ovsrec_port *port_row = NULL;
    const struct ovsrec_bridge *bridge_row = NULL;
    const struct ovsrec_vrf *vrf_row = NULL;

    struct ovsdb_idl_txn *status_txn = NULL;
    enum ovsdb_idl_txn_status status;

    struct ovsrec_port **vrf_port_list;
    struct ovsrec_port **bridge_port_list;

    bool intf_exist = false, port_exist = false;

    int i, j;

    status_txn = cli_do_config_start();

    if (!status_txn) {
        VLOG_ERR(
                "%s Got an error when trying to create a transaction"
                " using ovsdb_idl_txn_create()", __func__);
        cli_do_config_abort(status_txn);
        return CMD_OVSDB_FAILURE;
    }

    /*verify if interface exists */
    OVSREC_INTERFACE_FOR_EACH(if_row, idl)
    {
        if (strcmp(if_row->name, vlan_if) == 0) {
            intf_exist = true;
        }
    }

    /*verify if port exists */
    OVSREC_PORT_FOR_EACH(port_row, idl)
    {
        if (strcmp(port_row->name, vlan_if) == 0) {
            port_exist = true;
        }
    }

    /* If port OR interface does not exist return failure */
    if (intf_exist == false || port_exist == false) {
        vty_out(vty,
                "Vlan interface does not exist. Cannot delete %s",
                VTY_NEWLINE);
        cli_do_config_abort(status_txn);
        return CMD_OVSDB_FAILURE;
    }

    /* Remove the port row from vrf */
    /* Iterate through each VRF */
    OVSREC_VRF_FOR_EACH(vrf_row, idl)
    {
        vrf_port_list =
            xmalloc(sizeof(struct ovsrec_port) * (vrf_row->n_ports - 1));
        for (i = 0, j = 0; i < vrf_row->n_ports; i++) {
            if (strcmp(vrf_row->ports[i]->name, vlan_if) != 0) {
                vrf_port_list[j] = vrf_row->ports[i];
                j++;
            }
        }
        /* If we find the interface then update the vrf port list */
        if (i > j) {
            ovsrec_vrf_set_ports(vrf_row, vrf_port_list, vrf_row->n_ports - 1);
        }
        free(vrf_port_list);
    }

    /* Remove the port row from bridge */
    OVSREC_BRIDGE_FOR_EACH(bridge_row, idl)
    {
        bridge_port_list =
            xmalloc(sizeof(struct ovsrec_port) * (bridge_row->n_ports - 1));
        for (i = 0, j = 0; i < bridge_row->n_ports; i++) {
            if (strcmp(bridge_row->ports[i]->name, vlan_if) != 0) {
                bridge_port_list[j] = bridge_row->ports[i];
                j++;
            }
        }
        if (i > j) {
            ovsrec_bridge_set_ports(bridge_row, bridge_port_list,
                    bridge_row->n_ports - 1);
        }
        free(bridge_port_list);
    }

    status = cli_do_config_finish(status_txn);

    if (status == TXN_SUCCESS) {
        VLOG_DBG(
                "%s The command succeeded and interface \"%s\" was create",
                __func__, vlan_if);
        return CMD_SUCCESS;
    } else if (status == TXN_UNCHANGED) {
        VLOG_DBG(
                "%s The command resulted in no change "
                "Check if interface \"%s\" was create",
                __func__, vlan_if);
        return CMD_SUCCESS;
    } else {
        VLOG_ERR(
                "%s While trying to commit transaction to DB, got a status"
                " response : %s", __func__,
                ovsdb_idl_txn_status_to_string(status));
        return CMD_OVSDB_FAILURE;
    }
}

/* Function : verify_ifname. The function handles case sensitive commands
 *            Like VLAN10 or Vlan10. Also checks for invalid inputs like
 *            vlan 10aa or vlan 5000.
 * Description : verifies if the user input is valid.
 * param in : str - User passed Vlan interface name
 */
bool
verify_ifname(char *str)
{
    uint16_t vlanid = 0;
    char *endptr = NULL;

    if (VERIFY_VLAN_IFNAME(str) != 0) {
        return 0;
    }

    while(*str) {
        if (isdigit(*str)) {
            vlanid = strtol(str, &endptr, 10);
            VLOG_DBG("%s vlanid = %d, str = %s\n", __func__, vlanid, str);
            break;
        } else {
            str++;
            if (*str == '\0')
                return 0;
        }
    }

    /* For handling characters after/before <vlan id> */
    if (*endptr != '\0') {
        vty_out(vty, "Error : Invalid vlan input\n");
        return 0;
    }

    /* The VLANID is outside valid vlan range */
    if (vlanid <= 0 || vlanid >= 4095) {
        vty_out(vty, "Error : Vlanid outside valid vlan range <1-4094>\n");
        return 0;
    }

    /* The VLANID is internal vlan */
    if (check_internal_vlan(vlanid) == 0) {
        vty_out(vty, "Error : Vlanid is an internal vlan\n");
        return 0;
    }

    return 1;
}
#endif


/* Install Interface related vty commands. */
void
intf_vty_init (void)
{
    /* Config commands */
    install_element (INTERFACE_NODE, &cli_intf_shutdown_cmd);
    install_element (INTERFACE_NODE, &no_cli_intf_shutdown_cmd);
    install_element (INTERFACE_NODE, &cli_intf_split_cmd);
    install_element (INTERFACE_NODE, &no_cli_intf_split_cmd);
    install_element (INTERFACE_NODE, &cli_intf_speed_cmd);
    install_element (INTERFACE_NODE, &no_cli_intf_speed_cmd);
    install_element (INTERFACE_NODE, &cli_intf_mtu_cmd);
    install_element (INTERFACE_NODE, &no_cli_intf_mtu_cmd);
    install_element (INTERFACE_NODE, &cli_intf_duplex_cmd);
    install_element (INTERFACE_NODE, &no_cli_intf_duplex_cmd);
    install_element (INTERFACE_NODE, &cli_intf_flowcontrol_cmd);
    install_element (INTERFACE_NODE, &no_cli_intf_flowcontrol_cmd);
    install_element (INTERFACE_NODE, &cli_intf_autoneg_cmd);
    install_element (INTERFACE_NODE, &no_cli_intf_autoneg_cmd);

    /* Show commands */
    install_element (ENABLE_NODE, &cli_intf_show_intferface_ifname_cmd);
    install_element (ENABLE_NODE, &cli_intf_show_intferface_ifname_br_cmd);
    install_element (ENABLE_NODE, &cli_intf_show_run_intf_cmd);
    install_element (ENABLE_NODE, &cli_intf_show_run_intf_if_cmd);
    install_element (ENABLE_NODE, &cli_intf_show_run_intf_mgmt_cmd);

#ifdef ENABLE_OVSDB
    install_element (VLAN_INTERFACE_NODE, &cli_intf_shutdown_cmd);
    install_element (VLAN_INTERFACE_NODE, &no_cli_intf_shutdown_cmd);
#endif
    return;
}
