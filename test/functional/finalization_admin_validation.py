#!/usr/bin/env python3
# Copyright (c) 2018-2019 The Unit-e developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

from test_framework.util import (
    assert_equal,
    bytes_to_hex_str,
    connect_nodes,
    Decimal,
    hex_str_to_bytes,
    sync_mempools,
)
from test_framework.messages import CTransaction, msg_witness_tx, TxType
from test_framework.test_framework import UnitETestFramework
from test_framework.admin import Admin, AdminCommandType
from test_framework.mininode import P2PInterface
from io import BytesIO


class TestNode(P2PInterface):
    def __init__(self):
        super().__init__()
        self.reject_map = {}

    def on_reject(self, message):
        txid = "%064x" % message.data
        reason = "%s" % message.reason
        self.reject_map[txid] = reason

    def on_inv(self, message):
        pass


def create_tx(cmds, node, txid, vout, address, change_amount):
    inputs = [
        {
            "txid": txid,
            "vout": vout
        }
    ]

    outputs = {
        address: change_amount,
    }

    for i in range(len(cmds)):
        key = "data" + str(i)
        outputs[key] = cmds[i]

    return node.createrawtransaction(inputs, outputs)


def set_type_to_admin(raw_tx):
    tx = CTransaction()
    f = BytesIO(hex_str_to_bytes(raw_tx))
    tx.deserialize(f)

    tx.set_type(TxType.ADMIN)

    return bytes_to_hex_str(tx.serialize())


# Checks how system validates administrator commands.
# Only 2-of-3 multisig p2sh-segwit addresses with valid admin keys should be
# accepted. Admin txs should also contain at least one valid command
class AdminValidation(UnitETestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.extra_args = [
            ['-deprecatedrpc=signrawtransaction', '-deprecatedrpc=validateaddress', '-permissioning=1', '-debug=all', '-whitelist=127.0.0.1'],
            ['-deprecatedrpc=signrawtransaction', '-deprecatedrpc=validateaddress', '-permissioning=1', '-debug=all', '-whitelist=127.0.0.1']
        ]
        self.setup_clean_chain = True

    def setup_network(self):
        self.setup_nodes()
        connect_nodes(self.nodes[0], 1)

    def send_via_mininode(self, cmds, address):
        funds_tx = self.admin.sendtoaddress(address, Decimal("1"))
        self.wait_for_transaction(funds_tx, timeout=10)
        _, n = Admin.find_output_for_address(self.admin, funds_tx, address)

        raw_tx = create_tx(cmds, self.admin, funds_tx, n, address,
                           Decimal("0.09"))

        raw_tx = set_type_to_admin(raw_tx)

        raw_tx = self.admin.signrawtransaction(raw_tx)["hex"]

        tx = CTransaction()
        f = BytesIO(hex_str_to_bytes(raw_tx))
        tx.deserialize(f)

        self.admin.p2p.send_and_ping(msg_witness_tx(tx))

        tx.rehash()
        txid = tx.hash

        return txid

    # Sends commands to node and asserts that they were rejected
    def rejects(self, cmds, address, expected_reason):
        txid = self.send_via_mininode(cmds, address)

        reason = self.reject_tracker.reject_map.get(txid, None)
        if reason is None:
            raise AssertionError("Tx was not rejected! (txid=%s, expected=%s)" % (txid, expected_reason))
        assert_equal(expected_reason, reason)

    # Sends commands to node and asserts that they were accepted
    def accepts(self, cmds, address):
        txid = self.send_via_mininode(cmds, address)

        reason = self.reject_tracker.reject_map.get(txid, None)
        if reason is not None:
            raise AssertionError("Tx was rejected: " + reason)

    def create_new_multisig(self, n_signatures, n_keys, address_type):
        addresses = list(
            self.admin.getnewaddress("", address_type) for _ in range(n_keys))

        return self.admin.addmultisigaddress(n_signatures, addresses, "",
                                             address_type)["address"]

    def run_test(self):
        self.generator = self.nodes[0]
        self.admin = self.nodes[1]
        self.reject_tracker = TestNode()
        self.admin.add_p2p_connection(self.reject_tracker)

        self.setup_stake_coins(self.generator, self.admin)

        assert_equal(self.generator.initial_stake, self.generator.getbalance())
        assert_equal(self.admin.initial_stake, self.admin.getbalance())

        # Exit IBD
        self.generate_sync(self.generator)

        self.admin.p2p.wait_for_verack()

        end_permissioning_cmd = Admin.create_raw_command(
            AdminCommandType.END_PERMISSIONING)

        # Because: p2pkh, non-multisig
        self.rejects([end_permissioning_cmd],
                     self.admin.getnewaddress("", "legacy"),
                     "b'admin-invalid-witness'")
        # Because: non-multisig
        self.rejects([end_permissioning_cmd],
                     self.admin.getnewaddress("", "p2sh-segwit"),
                     "b'admin-invalid-witness'")
        # Because: invalid admin keys are not imported yet
        self.rejects([end_permissioning_cmd],
                     self.create_new_multisig(2, 3, "p2sh-segwit"),
                     "b'admin-not-authorized'")

        admin_addresses = Admin.import_admin_keys(self.admin)

        # Because: non-multisig
        self.rejects([end_permissioning_cmd],
                     admin_addresses[0],
                     "b'admin-invalid-witness'")

        # Because: 2-of-2 multisig, non-segwit
        self.rejects([end_permissioning_cmd],
                     self.admin.addmultisigaddress(2, admin_addresses[0:2], "",
                                                   "legacy")["address"],
                     "b'admin-invalid-witness'")

        # Because: 2-of-2 multisig
        self.rejects([end_permissioning_cmd],
                     self.admin.addmultisigaddress(2, admin_addresses[0:2], "",
                                                   "p2sh-segwit")["address"],
                     "b'admin-invalid-witness'")

        admin_address = self.admin.addmultisigaddress(2, admin_addresses, "",
                                                      "p2sh-segwit")["address"]

        # Because: no commands
        self.rejects([],
                     admin_address,
                     "b'admin-no-commands'")

        # Because: disables permissioning twice
        self.rejects([end_permissioning_cmd, end_permissioning_cmd],
                     admin_address,
                     "b'admin-double-disable'")

        # Because: command is invalid
        self.rejects(["1234"],
                     admin_address,
                     "b'admin-invalid-command'")

        # Because: second command is invalid
        self.rejects([end_permissioning_cmd, "1234"],
                     admin_address,
                     "b'admin-invalid-command'")

        # This is to ensure end_permissioning was not applied
        self.generate_sync(self.generator)

        # Going to reset admin keys. Generate new keys first
        new_addresses = list(self.admin.getnewaddress() for _ in range(3))
        new_pubkeys = list(
            self.admin.validateaddress(address)["pubkey"] for address in
            new_addresses)

        reset_admin_cmd = Admin.create_raw_command(
            AdminCommandType.RESET_ADMINS, new_pubkeys)

        self.accepts([reset_admin_cmd], admin_address)

        # Since we're sending the transactions to one node and generating
        # blocks on a different one, we need to sync their mempools
        sync_mempools(self.nodes)

        # Admin commands have no power until included into block
        self.generate_sync(self.generator)

        # Previous command has changed admin keys. Old address is invalid
        self.rejects([end_permissioning_cmd],
                     admin_address,
                     "b'admin-not-authorized'")

        admin_address = self.admin.addmultisigaddress(2, new_addresses, "",
                                                      "p2sh-segwit")["address"]

        self.accepts([end_permissioning_cmd], admin_address)

        sync_mempools(self.nodes)

        self.generate_sync(self.generator)  # to actually execute above command

        self.rejects([end_permissioning_cmd],
                     admin_address,
                     "b'admin-disabled'")


if __name__ == '__main__':
    AdminValidation().main()
