// SPDX-License-Identifier: GPL-3.0-or-later

//! Frame operations on an SHP's frame list: insert blank frames, duplicate,
//! delete, move and reverse. An operation only says which old frame each new
//! frame shows ([`FrameOp::order`]); the sheet is reflowed with
//! [`crate::sheet::reflow`] and the writer pairs every frame with the file
//! frame it came from, so untouched frames keep their bytes whatever their
//! new position.

use crate::codec::MAX_FRAMES;

/// One operation on the frame list. Frame numbers are 0-based.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FrameOp {
    /// `count` blank frames before frame `at` (`at` = the frame count
    /// appends them).
    Insert { at: usize, count: usize },
    /// A copy of frame `frame`, right after it.
    Duplicate { frame: usize },
    /// Remove frames `first..first + count`.
    Delete { first: usize, count: usize },
    /// Take frame `from` out and put it back so that it ends up at `to`.
    Move { from: usize, to: usize },
    /// Reverse the order of frames `first..first + count`.
    Reverse { first: usize, count: usize },
}

impl FrameOp {
    /// The frame list after the operation on `total` frames: for each new
    /// frame, the old frame it shows (`None` for a blank one). `None` when
    /// the operation does not apply: out of range, no frame left, more than
    /// an SHP holds, or nothing would change.
    pub fn order(&self, total: usize) -> Option<Vec<Option<usize>>> {
        let mut order: Vec<Option<usize>> = (0..total).map(Some).collect();
        match *self {
            FrameOp::Insert { at, count } => {
                if at > total || count == 0 {
                    return None;
                }
                order.splice(at..at, std::iter::repeat_n(None, count));
            }
            FrameOp::Duplicate { frame } => {
                if frame >= total {
                    return None;
                }
                order.insert(frame + 1, Some(frame));
            }
            FrameOp::Delete { first, count } => {
                let end = first.checked_add(count)?;
                if count == 0 || end > total || count >= total {
                    return None;
                }
                order.drain(first..end);
            }
            FrameOp::Move { from, to } => {
                if from >= total || to >= total || from == to {
                    return None;
                }
                let moved = order.remove(from);
                order.insert(to, moved);
            }
            FrameOp::Reverse { first, count } => {
                let end = first.checked_add(count)?;
                if count < 2 || end > total {
                    return None;
                }
                order[first..end].reverse();
            }
        }
        (order.len() <= usize::from(MAX_FRAMES)).then_some(order)
    }

    /// The frame to show after the operation: the first inserted, the copy,
    /// the frame now at the deleted position, the moved frame, or the first
    /// of the reversed range. `total` is the count after it.
    pub fn focus(&self, total: usize) -> usize {
        let frame = match *self {
            FrameOp::Insert { at, .. } => at,
            FrameOp::Duplicate { frame } => frame + 1,
            FrameOp::Delete { first, .. } => first,
            FrameOp::Move { to, .. } => to,
            FrameOp::Reverse { first, .. } => first,
        };
        frame.min(total.saturating_sub(1))
    }

    /// What History calls it.
    pub fn label(&self) -> &'static str {
        match *self {
            FrameOp::Insert { count: 1, .. } => "Insert Frame",
            FrameOp::Insert { .. } => "Insert Frames",
            FrameOp::Duplicate { .. } => "Duplicate Frame",
            FrameOp::Delete { count: 1, .. } => "Delete Frame",
            FrameOp::Delete { .. } => "Delete Frames",
            FrameOp::Move { .. } => "Move Frame",
            FrameOp::Reverse { .. } => "Reverse Frames",
        }
    }

    /// Parse the shell's spelling: `insert`, `duplicate`, `delete`, `move`,
    /// `reverse`, with `first`, `count` and (for `move`) `to`.
    pub fn parse(name: &str, first: usize, count: usize, to: usize) -> Option<FrameOp> {
        Some(match name {
            "insert" => FrameOp::Insert { at: first, count },
            "duplicate" => FrameOp::Duplicate { frame: first },
            "delete" => FrameOp::Delete { first, count },
            "move" => FrameOp::Move { from: first, to },
            "reverse" => FrameOp::Reverse { first, count },
            _ => return None,
        })
    }
}
